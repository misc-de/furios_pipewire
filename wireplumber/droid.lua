-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Droid monitor for WirePlumber.
--
-- Loads the SPA device api.droid.device and turns its objects into adapter
-- nodes. The detour through WirePlumber is necessary: if PipeWire instantiates
-- the device directly from context.objects, the result is raw SPA nodes
-- without an adapter - those have no format conversion and do not show up in
-- pipewire-pulse as a sink or source.
--
-- Second job: carry routes and call mode across the process boundary. The
-- device lives here in WirePlumber, the nodes run in the PipeWire daemon - and
-- only the node holds the HAL stream. So both go to the node as an
-- SPA_PROP_params pair, which the node translates into HAL calls.

cutils = require ("common-utils")
log = Log.open_topic ("s-monitors")

-- Last delivered state. The events also fire when nothing has changed - the
-- HAL should not notice that.
last_route = {}
last_route_volume = {}
last_mode = nil
last_volume = nil

-- Volume changes do not arrive as a param event on the node - they go
-- through WirePlumber's mixer API.
mixer = nil

function findNode (dev, card_profile_device)
  -- card.profile.device is a plain node property, not a global one - without
  -- type = "pw" the lookup finds nothing.
  return cutils.get_object_manager ("node"):lookup {
    Constraint { "device.id", "=", tostring (dev["bound-id"]) },
    Constraint { "card.profile.device", "=", tostring (card_profile_device),
                 type = "pw" },
  }
end

function setNodeProp (node, key, value)
  node:set_param ("Props", Pod.Object {
    "Spa:Pod:Object:Param:Props", "Props",
    params = Pod.Struct { key, value }
  })
end

-- The profile "voicecall" means AUDIO_MODE_IN_CALL to the HAL. Only the
-- playback node can set the mode: it holds the HAL module and the primary
-- output stream that the HAL reroutes on a mode change.
function forwardProfile (dev)
  for p in dev:iterate_params ("Profile") do
    local profile = cutils.parseParam (p, "Profile")
    if profile and profile.name then
      local mode = "normal"
      if profile.name == "voicecall" then
        mode = "call"
      elseif profile.name == "communication" then
        -- AUDIO_MODE_IN_COMMUNICATION: this makes the HAL turn on its echo
        -- cancellation. Meant for VoIP.
        mode = "communication"
      end
      if last_mode ~= mode then
        local node = findNode (dev, 0)
        if node then
          last_mode = mode
          log:info ("droid: audio mode " .. mode)
          setNodeProp (node, "droid.mode", mode)
          -- The HAL does not know the current level yet.
          last_volume = nil
          forwardVolume (node)

          -- The capture node needs to hear about it too. Not to set the mode -
          -- that belongs to playback - but because the HAL takes the mix
          -- port's audio source for the call and never gives it back. Without
          -- this the microphone reads digital silence after every call, until
          -- something restarts the audio stack.
          local src = findNode (dev, 1)
          if src then
            setNodeProp (src, "droid.mode", mode)
          end
        end
      end
    end
  end
end

-- Volume during a call. Otherwise the adapter handles it in software - but
-- during a call no PCM flows through the graph, the level lives in the HAL.
function forwardVolume (node)
  if last_mode ~= "call" or mixer == nil or node == nil then
    return
  end
  local vol = mixer:call ("get-volume", node["bound-id"])
  if vol == nil then
    return
  end

  local v = string.format ("%.3f", vol.mute and 0.0 or vol.volume)
  if last_volume ~= v then
    last_volume = v
    log:info ("droid: voice level " .. v)
    setNodeProp (node, "droid.voice-volume", v)
  end
end

-- The playback level travels with the route.
--
-- A node that belongs to a card takes its volume from the props of the active
-- route - that is where pipewire-pulse reads it, and a card whose routes carry
-- none reports 0 % to every PulseAudio client and drops what they set.
--
-- Reporting it there has a price: PipeWire then stops applying the level in
-- software, because it assumes the hardware does. This HAL does not. It accepts
-- set_volume on the primary output and returns success, but the measured level
-- does not move (RMS 5796 at 100 %, 5734 at 20 %) - on Android that gain sits
-- in AudioFlinger, above the HAL. So we put the level back onto the node, where
-- the graph applies it for real. The route keeps it for everyone who reads it.
function forwardRouteVolume (route, node)
  if node == nil or route.props == nil then
    return
  end
  -- parseParam hands back the pod wrapper, not the values: the props sit one
  -- level down under .properties. Reading route.props.volume directly gives
  -- nil, silently and without an error.
  local props = route.props.properties or route.props
  local cv = props.channelVolumes
  local v = (type (cv) == "table" and cv[1]) or props.volume
  if v == nil then
    return
  end
  local mute = props.mute and true or false

  local key = string.format ("%.4f/%s", v, tostring (mute))
  if last_route_volume[route.device] == key then
    return
  end
  last_route_volume[route.device] = key

  -- pcall: this runs in the monitor, and an error here takes the whole script
  -- down with it - which means no card, no nodes, no sound at all. A volume
  -- that fails to apply must not cost more than the volume.
  local ok, err = pcall (function ()
    local n = (type (cv) == "table" and #cv > 0) and #cv or 2
    local vols = {}
    for i = 1, n do vols[i] = v end
    node:set_param ("Props", Pod.Object {
      "Spa:Pod:Object:Param:Props", "Props",
      mute = mute,
      channelVolumes = Pod.Array { "Spa:Float", table.unpack (vols) },
    })
  end)
  if ok then
    log:info ("droid: level " .. key .. " for " .. tostring (route.name))
  else
    log:warning ("droid: level not applied: " .. tostring (err))
  end
end

function forwardRoutes (dev)
  for p in dev:iterate_params ("Route") do
    local route = cutils.parseParam (p, "Route")
    if route and route.device ~= nil and route.name then
      -- The volume changes far more often than the route, so the node has to
      -- be looked up even when the route itself stayed the same.
      local node = findNode (dev, route.device)
      if node and last_route[route.device] ~= route.name then
        last_route[route.device] = route.name
        log:info ("droid: route " .. route.name .. " to " ..
                  tostring (node.properties["node.name"]))
        setNodeProp (node, "droid.route", route.name)
      end
      forwardRouteVolume (route, node)
    end
  end
end

function forwardAll (dev)
  -- Mode first: when entering a call the HAL routes to the earpiece by
  -- itself. An explicit route choice should win afterwards.
  forwardProfile (dev)
  forwardRoutes (dev)
end

-- Profile and route changes arrive as events, not as a signal on the proxy.
device_hook = SimpleEventHook {
  name = "monitor/droid-forward-device-params",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Route" },
    },
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Profile" },
    },
  },
  execute = function (event)
    local dev = event:get_subject ()
    if dev.properties["device.api"] == "droid-hal" then
      forwardAll (dev)
    end
  end
}

function createNode (parent, id, obj_type, factory, properties)
  -- Mandatory fields: without device.id the node belongs to no card, without
  -- factory.name the adapter does not know which SPA node to load.
  properties["device.id"] = parent["bound-id"]
  properties["factory.name"] = factory
  properties["device.api"] = "droid-hal"
  properties["node.pause-on-idle"] = false

  local node = Node ("adapter", properties)
  parent:set_managed_pending (id)
  node:activate (Feature.Proxy.BOUND, function (_, err)
    if err then
      log:warning ("droid: node " .. tostring (properties["node.name"]) ..
                   " failed: " .. tostring (err))
      return
    end
    parent:store_managed_object (id, node)
    -- The freshly created node does not know route and mode yet.
    local dev = cutils.get_object_manager ("device"):lookup {
      Constraint { "device.api", "=", "droid-hal" }
    }
    if dev then
      forwardAll (dev)
    end
  end)
end

device = SpaDevice ("api.droid.device", {
  ["device.name"] = "droid_card.primary",
  ["device.description"] = "Phone",
  ["device.nick"] = "droid",
  ["device.api"] = "droid-hal",
  ["media.class"] = "Audio/Device",
})

if device == nil then
  log:notice ("droid: SPA plugin api.droid.device could not be loaded")
  return
end

device:connect ("create-object", createNode)
device:connect ("object-removed", function (parent, id)
  parent:store_managed_object (id, nil)
end)
device:activate (Feature.SpaDevice.ENABLED | Feature.Proxy.BOUND)
device_hook:register ()

mixer = Plugin.find ("mixer-api")
if mixer then
  log:warning ("droid: mixer-api found")
  mixer:connect ("changed", function (m, id)
    log:warning ("droid: mixer changed id=" .. tostring (id) .. " mode=" .. tostring (last_mode))
    if last_mode ~= "call" then
      return
    end
    local node = cutils.get_object_manager ("node"):lookup {
      Constraint { "bound-id", "=", id, type = "gobject" }
    }
    if node and node.properties["device.api"] == "droid-hal" and
       node.properties["media.class"] == "Audio/Sink" then
      forwardVolume (node)
    end
  end)
else
  log:warning ("droid: mixer-api unavailable - call volume stays fixed")
end

log:info ("droid: device activated")
