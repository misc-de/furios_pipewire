-- Droid-Monitor fuer WirePlumber.
--
-- Bindet das SPA-Device api.droid.device ein und macht aus dessen Objekten
-- Adapter-Knoten. Der Umweg ueber WirePlumber ist noetig: laesst man PipeWire
-- das Device direkt aus context.objects instanziieren, entstehen rohe
-- SPA-Knoten ohne Adapter - die haben keine Formatwandlung und erscheinen in
-- pipewire-pulse nicht als Sink oder Source.
--
-- Zweite Aufgabe: Routen und Anrufmodus ueber die Prozessgrenze reichen. Das
-- Device lebt hier in WirePlumber, die Knoten laufen im PipeWire-Daemon - und
-- nur der Knoten haelt den HAL-Stream. Beides geht deshalb als
-- SPA_PROP_params-Paar an den Knoten, der es in HAL-Aufrufe uebersetzt.

cutils = require ("common-utils")
log = Log.open_topic ("s-monitors")

-- Zuletzt zugestellter Stand. Die Ereignisse feuern auch, wenn sich nichts
-- geaendert hat - der HAL soll davon nichts merken.
last_route = {}
last_mode = nil
last_volume = nil

-- Lautstaerkeaenderungen kommen nicht als Param-Ereignis am Knoten an -
-- sie laufen ueber WirePlumbers Mixer-API.
mixer = nil

function findNode (dev, card_profile_device)
  -- card.profile.device ist eine reine Knoteneigenschaft, keine globale -
  -- ohne type = "pw" findet der lookup nichts.
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

-- Das Profil "voicecall" bedeutet fuer den HAL AUDIO_MODE_IN_CALL. Setzen kann
-- den Modus nur der Wiedergabeknoten: er haelt das HAL-Modul und den primaeren
-- Ausgangsstream, den der HAL beim Moduswechsel umroutet.
function forwardProfile (dev)
  for p in dev:iterate_params ("Profile") do
    local profile = cutils.parseParam (p, "Profile")
    if profile and profile.name then
      local mode = "normal"
      if profile.name == "voicecall" then
        mode = "call"
      elseif profile.name == "communication" then
        -- AUDIO_MODE_IN_COMMUNICATION: der HAL schaltet dafuer seine
        -- Echounterdrueckung ein. Gedacht fuer VoIP.
        mode = "communication"
      end
      if last_mode ~= mode then
        local node = findNode (dev, 0)
        if node then
          last_mode = mode
          log:info ("droid: Audiomodus " .. mode)
          setNodeProp (node, "droid.mode", mode)
          -- Der HAL kennt den aktuellen Pegel noch nicht.
          last_volume = nil
          forwardVolume (node)
        end
      end
    end
  end
end

-- Lautstaerke im Gespraech. Der Adapter regelt sie sonst in Software - im
-- Anruf fliesst aber kein PCM durch den Graphen, der Pegel sitzt im HAL.
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
    log:info ("droid: Sprachlautstaerke " .. v)
    setNodeProp (node, "droid.voice-volume", v)
  end
end

function forwardRoutes (dev)
  for p in dev:iterate_params ("Route") do
    local route = cutils.parseParam (p, "Route")
    if route and route.device ~= nil and route.name then
      local node = nil
      if last_route[route.device] ~= route.name then
        node = findNode (dev, route.device)
      end
      if node then
        last_route[route.device] = route.name
        log:info ("droid: Route " .. route.name .. " an " ..
                  tostring (node.properties["node.name"]))
        setNodeProp (node, "droid.route", route.name)
      end
    end
  end
end

function forwardAll (dev)
  -- Erst der Modus: der HAL routet beim Wechsel in den Anruf selbst auf die
  -- Ohrmuschel. Eine ausdrueckliche Routenwahl soll danach gewinnen.
  forwardProfile (dev)
  forwardRoutes (dev)
end

-- Profil- und Routenwechsel kommen als Ereignis, nicht als Signal am Proxy.
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
  -- Pflichtfelder: ohne device.id gehoert der Knoten zu keiner Karte, ohne
  -- factory.name weiss der Adapter nicht, welchen SPA-Knoten er laden soll.
  properties["device.id"] = parent["bound-id"]
  properties["factory.name"] = factory
  properties["device.api"] = "droid-hal"
  properties["node.pause-on-idle"] = false

  local node = Node ("adapter", properties)
  parent:set_managed_pending (id)
  node:activate (Feature.Proxy.BOUND, function (_, err)
    if err then
      log:warning ("droid: Knoten " .. tostring (properties["node.name"]) ..
                   " fehlgeschlagen: " .. tostring (err))
      return
    end
    parent:store_managed_object (id, node)
    -- Der frisch angelegte Knoten kennt Route und Modus noch nicht.
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
  ["device.description"] = "Android HAL",
  ["device.nick"] = "droid",
  ["device.api"] = "droid-hal",
  ["media.class"] = "Audio/Device",
})

if device == nil then
  log:notice ("droid: SPA-Plugin api.droid.device nicht ladbar")
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
  log:warning ("droid: mixer-api gefunden")
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
  log:warning ("droid: mixer-api nicht verfuegbar - Lautstaerke im Anruf bleibt fest")
end

log:info ("droid: Device aktiviert")
