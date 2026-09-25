-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Put a phone call on the Bluetooth headset without anyone typing anything.
--
-- On this device the voice path of a mobile call never reaches the host: it
-- runs modem <-> DSP. A headset is served by the HAL instead, which puts the
-- path onto the Bluetooth PCM line when it is routed to a BT SCO device
-- (set_parameters BT_SCO=on). Two things have to be true at once for that:
--
--   1. the headset is in a hands-free profile, so an SCO channel exists at all
--   2. the phone card is routed to output-bluetooth_sco / input-bluetooth_sco_headset
--   3. something holds an SCO link open for the length of the call
--
-- The third one is not done here yet, and without it the other two are not
-- enough: measured 2026-09-12, a call with the profile and both routes set
-- correctly and held steady for the whole call was silent in both directions.
-- A hands-free PROFILE is not a hands-free LINK. The link exists only while a
-- stream is active on bluez_output.*, and in a call nobody opens one - the
-- voice path runs modem <-> DSP and never reaches the host, so there is no
-- stream to be had. The same call with "audioctl bt-mic on" holding a stream
-- of zeroes on bluez_output underneath it was heard, both directions.
--
-- Do not look for that in /proc/asound/card0/pcm55p|pcm55c. Those stay closed
-- through a call that is working and being heard; they show host streams, not
-- telephony. For a call the ear is the instrument.
--
-- "audioctl bt-call watch" does both, but it has to be started before the call.
-- This does the same as a hook: it reacts when the card enters the voicecall
-- profile, and puts everything back when the call ends.
--
-- THIS IS OFF BY DEFAULT, and the reason is a real call that had no audio at
-- all - not on the headset, not on the earpiece, not in either direction.
-- callaudiod resets the port during a call because it only knows earpiece and
-- speaker; this script set it back; callaudiod set it again. Two to three
-- round trips a second, and the HAL tore the voice path down and rebuilt it
-- every time (BT_SCO=off, BT_SCO=on, ...). Picking the earpiece by hand did
-- not help either, because this script overrode that too.
--
-- So: the automatic routing only runs when the setting
-- furios.bluetooth-call-routing is on (see 51-bluez-ofono.conf), and even
-- then it gives up after a few rounds of that fight and hands the call back
-- to the phone. A call on the earpiece is a nuisance. A call with no audio is
-- not, and the phone has to keep working.
--
-- Everything here is wrapped in pcall. This runs inside the monitor, and an
-- error would take the whole script down with it: no card, no nodes, no sound.
-- A call that stays on the earpiece is a nuisance; a call with no audio at all
-- is not, and the phone must survive whatever this gets wrong.

cutils = require ("common-utils")
log = Log.open_topic ("s-node")

BT_SINK_ROUTE   = "output-bluetooth_sco"
BT_SOURCE_ROUTE = "input-bluetooth_sco_headset"

SETTING = "furios.bluetooth-call-routing"

-- How often the route may be pushed away before this gives up for the rest of
-- the call. Three is enough to survive callaudiod setting the port once or
-- twice while the call is being set up, and short enough that a fight is over
-- in well under a second.
MAX_DEFENDS = 3

-- How long the card has to be QUIET before touching the Bluetooth side.
--
-- callaudiod runs a sequence of PulseAudio operations when a call starts -
-- park the output, set the real port, set the input port - and it waits for
-- each one. Changing the route or the Bluetooth profile in the middle of that
-- changes the cards underneath it, and its next SelectMode then blocks until
-- the D-Bus timeout: 25 seconds with no audio in either direction.
--
-- A fixed delay from the first sight of the call is not enough, and a real
-- call showed why: the profile goes to voicecall while the phone is still
-- ringing, so a delay started there elapsed in the middle of callaudiod's
-- work. Measured: the takeover landed 14 ms before callaudiod set the port.
--
-- So the wait is not for a span of time but for quiet. Every route or profile
-- change while the call is being set up starts it again, and the takeover
-- happens only when nothing has moved for this long. callaudiod's own
-- sequence is about 200 ms end to end, so a second of silence means it is
-- finished.
QUIET_MS = 1000

-- How often to look at the codec while the link is being built, and how long
-- that counts as "being built".
--
-- The codec is not settled when the profile is: BlueZ agrees it when the SCO
-- link is acquired, and on this phone that is furios-audio-sco-hold opening a
-- stream a second or two into the call. So the first reading can be the
-- preferred codec rather than the agreed one, and the difference between the
-- two is silence. Reading it again costs nothing and the HAL reopens its
-- stream by itself when the answer changes (droid-pcm.c, apply_bt_wbs).
--
-- Slowly for the rest of the call, because a link that drops and comes back
-- may come back on the other codec, and a call is minutes long.
CODEC_POLL_MS = 250
CODEC_FAST_FOR_MS = 15000
CODEC_SLOW_MS = 2000

in_bt_call = false
gave_up = false
defends = 0
saved_routes = nil

-- Bumped by every event while we are waiting. A timer that fires with a stale
-- number knows something moved after it was armed, and stands down.
quiet_token = 0
took_over = false

-- What the phone's nodes were last told, and a token for the codec watch that
-- works like quiet_token: a round that fires with a stale one is from a call
-- that is over.
announced_wbs = nil
codec_token = 0

-- The hands-free profile the call was put on, so the hook below knows which
-- one to keep.
call_profile = nil

-- Off unless someone turned it on. An unknown setting, an older WirePlumber,
-- anything unexpected: all of that has to come out as "leave the call alone".
function autoRoutingEnabled ()
  local ok, value = pcall (function ()
    return Settings.get_boolean (SETTING)
  end)
  return ok and value == true
end

function btCard ()
  return cutils.get_object_manager ("device"):lookup {
    Constraint { "device.api", "=", "bluez5" },
  }
end

function droidCard ()
  return cutils.get_object_manager ("device"):lookup {
    Constraint { "device.api", "=", "droid-hal" },
  }
end

function inVoiceCall (dev)
  for p in dev:iterate_params ("Profile") do
    local profile = cutils.parseParam (p, "Profile")
    if profile and profile.name == "voicecall" then
      return true
    end
  end
  return false
end

-- Route names of what is active right now, per direction.
function activeRoutes (dev)
  local out = {}
  for p in dev:iterate_params ("Route") do
    local r = cutils.parseParam (p, "Route")
    if r and r.device ~= nil and r.name then
      out[r.device] = r.name
    end
  end
  return out
end

function setRouteByName (dev, name)
  for p in dev:iterate_params ("EnumRoute") do
    local r = cutils.parseParam (p, "EnumRoute")
    if r and r.name == name and r.devices ~= nil then
      for _, d in ipairs (r.devices) do
        dev:set_params ("Route", Pod.Object {
          "Spa:Pod:Object:Param:Route", "Route",
          index = r.index,
          device = d,
          -- Never stored: this is where the call goes, not a choice the owner
          -- made about their phone.
          save = false,
        })
      end
      return true
    end
  end
  return false
end

-- The headset has to be in a hands-free profile, otherwise there is no SCO
-- channel for the HAL to put the voice path on.
--
-- Returns the profile name on success, so the caller knows which codec was
-- agreed - see tellCodec below, which needs exactly that.
function setBtProfile (card, name)
  if card == nil then
    return nil
  end
  for p in card:iterate_params ("EnumProfile") do
    local profile = cutils.parseParam (p, "EnumProfile")
    if profile and profile.name == name then
      card:set_params ("Profile", Pod.Object {
        "Spa:Pod:Object:Param:Profile", "Profile",
        index = profile.index,
        save = false,
      })
      return name
    end
  end
  return nil
end

-- Tell the phone's nodes which codec the Bluetooth link runs, because nothing
-- else can.
--
-- The HAL encodes narrow-band CVSD unless it is told otherwise. Mismatched in
-- either direction the far end decodes the wrong thing and plays nothing -
-- while everything measurable says the audio is on its way: the link stands,
-- BTCVSD Tx Irq is on, the HAL holds the Bluetooth PCM device. Only the ear
-- notices. Measured, same tone each time: mSBC air + HAL default, nothing;
-- CVSD air + HAL default, heard; mSBC air + bt_wbs=on, heard.
--
-- This used to read the codec off the profile NAME - headset-head-unit as
-- mSBC, headset-head-unit-cvsd as CVSD - and that is wrong, because the name
-- of the plain profile says nothing about the codec. libspa-bluez5 builds the
-- per-codec names by appending the codec (bluez5-device.c: "%s-%s") and keeps
-- headset-head-unit for whichever codec the two ends agree on; its
-- description then reads "Headset Head Unit (HSP/HFP, codec CVSD)" or
-- "... codec MSBC". takeOver asks for the plain one, so it got mSBC announced
-- for every hands-free device on earth.
--
-- It cost a car. 2026-09-18 23:02, hands-free in the car: profile, routes,
-- SCO hold, all of it right, bt_wbs=on announced two seconds in - and the
-- call was silent in both directions. The car is an HFP 1.5 unit whose SDP
-- record has SupportedFeatures 0x0007: no wide-band speech bit, so the link
-- could only ever be CVSD. The earbuds that this was built and heard with are
-- HFP 1.7, 0x003F, wide-band bit set. One bit apart, and nothing in the code
-- ever looked at it.
--
-- So ask what the link is actually running, and say nothing when that cannot
-- be answered - the HAL then keeps its narrow-band default, which is a better
-- failure than a wrong guess: the guess is silence that measures perfectly.
--
-- It goes straight to the nodes, the way droid.lua sends the route. The card
-- is not a road: it lives in WirePlumber's process and the nodes in
-- PipeWire's, and nothing carries a card parameter across that on its own -
-- measured, the node receives droid.route (which droid.lua sends) and nothing
-- else the card publishes.
--
-- Both directions are told: bt_wbs belongs to the HAL module rather than to
-- one stream, and either node may be the next to open one.
--
-- And it should happen BEFORE the routes are set. The HAL reads the parameter
-- when it opens a stream, and it is the route that makes it open; told later
-- it reopens the stream, which works but costs a gap in the call.
function nodeOf (dev, card_profile_device)
  return cutils.get_object_manager ("node"):lookup {
    Constraint { "device.id", "=", tostring (dev["bound-id"]) },
    Constraint { "card.profile.device", "=", tostring (card_profile_device),
                 type = "pw" },
  }
end

function tellCodec (dev, wbs)
  if wbs == nil then
    return nil
  end

  local told = 0
  for _, d in ipairs { 0, 1 } do
    local node = nodeOf (dev, d)
    if node then
      node:set_param ("Props", Pod.Object {
        "Spa:Pod:Object:Param:Props", "Props",
        params = Pod.Struct { "droid.bt-wbs", wbs },
      })
      told = told + 1
    end
  end
  if told == 0 then
    return nil
  end
  announced_wbs = wbs
  return wbs
end

-- A codec name as libspa-bluez5 writes it, turned into what the HAL takes.
--
-- Only these two exist for it: bt_wbs is a yes-or-no about mSBC. A link on
-- LC3-SWB, or an A2DP node still carrying "sbc" or "aac" from before the
-- profile switch, is not something this can answer - and a wrong answer is
-- silence, so those come back as "do not know" and the HAL keeps its default.
function wbsOfCodec (name)
  if name == nil then
    return nil
  end
  name = string.lower (tostring (name))
  if name == "msbc" then
    return "on"
  elseif name == "cvsd" then
    return "off"
  end
  return nil
end

-- What the link really runs, from the node that runs it.
--
-- The Bluetooth nodes carry api.bluez5.codec, and after the profile switch
-- that is the codec BlueZ negotiated - not what anyone preferred. It is the
-- only source here that can tell one headset from another.
function codecOfNodes (card)
  if card == nil then
    return nil
  end
  -- The constraint is written exactly as nodeOf writes it, because that one
  -- is known to match on the phone.
  local om = cutils.get_object_manager ("node")
  for node in om:iterate {
      Constraint { "device.id", "=", tostring (card["bound-id"]) },
    } do
    local wbs = wbsOfCodec (node.properties ["api.bluez5.codec"])
    if wbs then
      return wbs, node.properties ["api.bluez5.codec"]
    end
  end
  -- Some builds put it on the card instead of on its nodes.
  local wbs = wbsOfCodec (card.properties and card.properties ["api.bluez5.codec"])
  if wbs then
    return wbs, card.properties ["api.bluez5.codec"]
  end
  return nil
end

-- What the card says about the profile that was just set.
--
-- Two readings, both from PipeWire itself: the description, which is built as
-- "Headset Head Unit (HSP/HFP, codec %s)" and names the codec the profile
-- stands for, and the name, which carries it as a suffix on the per-codec
-- profiles (headset-head-unit-msbc, -cvsd). The plain headset-head-unit says
-- nothing by its name - it is whichever codec the two ends agreed on - and
-- reading it as mSBC is exactly the mistake this replaces.
function codecOfProfile (card, name)
  if card == nil or name == nil then
    return nil
  end
  for p in card:iterate_params ("EnumProfile") do
    local profile = cutils.parseParam (p, "EnumProfile")
    if profile and profile.name == name then
      local said = profile.description
                   and string.match (tostring (profile.description), "codec%s+([%a%d_-]+)")
      local wbs = wbsOfCodec (said)
      if wbs then
        return wbs, said
      end
    end
  end
  local suffix = string.match (name, "^headset%-head%-unit%-(.+)$")
  local wbs = wbsOfCodec (suffix)
  if wbs then
    return wbs, suffix
  end
  return nil
end

-- The codec, from the best source that can answer right now.
function btWbs (card, profile)
  local wbs, name = codecOfNodes (card)
  if wbs then
    return wbs, name .. " (from the link)"
  end
  wbs, name = codecOfProfile (card, profile)
  if wbs then
    return wbs, name .. " (from the profile)"
  end
  return nil
end

-- Look again while the call runs, because the answer can arrive late.
--
-- Nothing is forced on the HAL from here: told a codec it already has, the
-- plugin does nothing, and told a different one under an open Bluetooth
-- stream it reopens that stream itself.
function watchCodec (dev, card, elapsed)
  codec_token = codec_token + 1
  local mine = codec_token
  local delay = elapsed < CODEC_FAST_FOR_MS and CODEC_POLL_MS or CODEC_SLOW_MS

  Core.timeout_add (delay, function ()
    -- A newer round, or the call is over: this one has nothing to do.
    if mine ~= codec_token or not in_bt_call then
      return false
    end

    local ok, err = pcall (function ()
      local wbs, from = codecOfNodes (card)
      if wbs and wbs ~= announced_wbs then
        log:info ("bluetooth call: the link runs " .. tostring (from) ..
                  " - telling the phone bt_wbs=" .. wbs ..
                  (announced_wbs and " instead of " .. announced_wbs or ""))
        tellCodec (dev, wbs)
      end
    end)
    if not ok then
      -- Stop looking rather than take WirePlumber down with it. The call
      -- keeps whatever it was told at the start.
      log:warning ("bluetooth call: watching the codec failed - " .. tostring (err))
      return false
    end

    watchCodec (dev, card, elapsed + delay)
    return false
  end)
end

function takeOver (dev, card)
  took_over = true
  local prof = setBtProfile (card, "headset-head-unit")
             or setBtProfile (card, "headset-head-unit-cvsd")
  call_profile = prof
  -- Before the routes: setting a route is what makes the HAL open a stream,
  -- and the codec should be in place by then.
  local wbs, from = btWbs (card, prof)
  wbs = tellCodec (dev, wbs)
  local sink = setRouteByName (dev, BT_SINK_ROUTE)
  local src  = setRouteByName (dev, BT_SOURCE_ROUTE)

  log:info (string.format (
      "bluetooth call: headset profile %s, codec %s, output %s, input %s",
      prof or "NOT set",
      wbs and ("bt_wbs=" .. wbs .. ", " .. tostring (from))
           or "unknown - the HAL keeps its narrow-band default",
      sink and "set" or "NOT set", src and "set" or "NOT set"))

  -- The codec is agreed when the SCO link goes up, which is after this.
  watchCodec (dev, card, 0)

  if not sink then
    -- Without the output route the HAL never sends BT_SCO=on, and holding a
    -- half-applied state would be worse than not trying.
    in_bt_call = false
    saved_routes = nil
  end
end

function enterBtCall (dev, card)
  saved_routes = activeRoutes (dev)
  in_bt_call = true
  took_over = false
  defends = 0
  -- Nothing is known about this call's codec yet, and the last call's answer
  -- is about a headset that may not even be the same one.
  announced_wbs = nil
  codec_token = codec_token + 1
  waitForQuiet (dev, card)
end

-- Arm the takeover, and re-arm it whenever anything moves.
function waitForQuiet (dev, card)
  quiet_token = quiet_token + 1
  local mine = quiet_token

  Core.timeout_add (QUIET_MS, function ()
    -- Something moved after this timer was armed, or the call ended: either
    -- way this one is not the timer that acts.
    if mine ~= quiet_token or not in_bt_call then
      return false
    end
    local ok, err = pcall (function () takeOver (dev, card) end)
    if not ok then
      in_bt_call = false
      saved_routes = nil
      log:warning ("bluetooth call: giving up - " .. tostring (err))
    end
    return false
  end)
end

function leaveBtCall (dev, card)
  in_bt_call = false
  codec_token = codec_token + 1
  if saved_routes then
    for _, name in pairs (saved_routes) do
      setRouteByName (dev, name)
    end
    saved_routes = nil
  end
  setBtProfile (card, "a2dp-sink")
  log:info ("bluetooth call: over, back to the phone")
end

-- Hand the call back to the phone and do not try again until it is over.
function giveUp (dev, card, why)
  -- Say what was on the route instead. When this fires, the interesting
  -- question is who keeps taking it away, and the name is the only clue there
  -- is from inside here.
  local found = {}
  for _, name in pairs (activeRoutes (dev)) do
    table.insert (found, name)
  end
  log:warning ("bluetooth call: " .. why .. " (route is on " ..
               (next (found) and table.concat (found, ", ") or "nothing") ..
               ") - the call goes back to the phone")
  in_bt_call = false
  gave_up = true
  defends = 0
  codec_token = codec_token + 1
  if saved_routes then
    for _, name in pairs (saved_routes) do
      setRouteByName (dev, name)
    end
    saved_routes = nil
  end
  setBtProfile (card, "a2dp-sink")
end

-- callaudiod sets the port back to the earpiece during the call. Put it back -
-- but not forever. callaudiod does not yield, and a route that changes two or
-- three times a second means the HAL rebuilds the voice path just as often;
-- the call then carries no audio in either direction, which is worse than a
-- call that stayed on the phone.
function defendRoute (dev, card)
  local active = activeRoutes (dev)
  for _, name in pairs (active) do
    if name == BT_SINK_ROUTE then
      return
    end
  end

  -- Counted on every attempt, not on every observed loss. Reading the card and
  -- acting on what it said is a race against the other side setting it back,
  -- and losing that race meant the count stayed at zero while the two of us
  -- traded the route several times a second - which is exactly the state this
  -- is here to end.
  defends = defends + 1
  if defends > MAX_DEFENDS then
    giveUp (dev, card, "the route will not stay on the headset")
    return
  end

  log:info ("bluetooth call: route moved away, setting it again")
  setRouteByName (dev, BT_SINK_ROUTE)
  setRouteByName (dev, BT_SOURCE_ROUTE)
end

bluetooth_call_hook = SimpleEventHook {
  name = "monitor/droid-bluetooth-call",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Profile" },
    },
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Route" },
    },
  },
  execute = function (event)
    local dev = event:get_subject ()
    local api = dev.properties["device.api"]

    -- A headset that is connected while the call is already running.
    --
    -- The phone card emits nothing at all in that case - its profile and its
    -- routes are exactly as they were - so waiting for it means waiting
    -- forever. The Bluetooth card, on the other hand, announces itself as it
    -- settles, and that is the only moment there is to notice. Everything
    -- after this point works on the phone card, as before.
    if api == "bluez5" then
      dev = droidCard ()
      if dev == nil then
        return
      end
    elseif api ~= "droid-hal" then
      return
    end

    local ok, err = pcall (function ()
      local call = inVoiceCall (dev)
      local card = btCard ()

      if call and not in_bt_call and not gave_up then
        -- No headset, nothing to do: the call stays on the phone, which is
        -- exactly right. Same when the automatic routing is switched off.
        if card ~= nil and autoRoutingEnabled () then
          enterBtCall (dev, card)
        end
      elseif not call and in_bt_call then
        leaveBtCall (dev, card)
      elseif not call and gave_up then
        -- Already handed back mid-call; the call is over, so try again next
        -- time.
        gave_up = false
      elseif call and in_bt_call then
        if saved_routes ~= nil and not took_over then
          -- Still waiting for callaudiod to finish: anything that moves
          -- starts the wait again.
          waitForQuiet (dev, card)
        else
          defendRoute (dev, card)
        end
      end
    end)
    if not ok then
      in_bt_call = false
      gave_up = false
      defends = 0
      saved_routes = nil
      log:warning ("bluetooth call: giving up - " .. tostring (err))
    end
  end,
}

bluetooth_call_hook:register ()

-- Keep the headset in hands-free for the length of the call.
--
-- WirePlumber picks a profile afresh whenever a card's list of profiles
-- changes (device/select-profile), and what it picks is the stored one or the
-- best one - for a headset that is A2DP, priority 133 against 3. The list
-- changes when music was playing just before the call: the A2DP transport is
-- released as the profile goes to hands-free, the set of connected profiles
-- moves, and 330 ms after takeOver WirePlumber put the card straight back on
-- a2dp-sink. Measured 2026-09-25 18:08:20 with spa.bluez5 at debug level:
-- "setting profile 2 codec:4 save:0". furios-audio-sco-hold then held its
-- stream of zeroes on the A2DP sink instead of the hands-free one, no SCO
-- link ever came up (hcitool con: ACL only, no eSCO), and three calls in a
-- row had no sound and no microphone. The one call that worked that day had
-- no music before it - nothing was released, nothing was re-picked.
--
-- So while a call is on the headset, the answer to "which profile" is the one
-- the call is on. This runs after every hook that makes a choice and before
-- the one that applies it; outside a call it does nothing at all.
keep_call_profile_hook = SimpleEventHook {
  name = "device/furios-keep-call-profile",
  after = { "device/find-calling-profile", "device/find-stored-profile",
            "device/find-preferred-profile", "device/find-best-profile" },
  before = "device/apply-profile",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-profile" },
    },
  },
  execute = function (event)
    local ok, err = pcall (function ()
      if not in_bt_call or not took_over or call_profile == nil then
        return
      end
      local card = event:get_subject ()
      if card.properties["device.api"] ~= "bluez5" then
        return
      end
      for p in card:iterate_params ("EnumProfile") do
        local profile = cutils.parseParam (p, "EnumProfile")
        if profile and profile.name == call_profile then
          local picked = event:get_data ("selected-profile")
          if picked == nil or picked.name ~= call_profile then
            log:info ("bluetooth call: keeping the headset on " .. call_profile ..
                      " instead of " .. tostring (picked and picked.name))
          end
          event:set_data ("selected-profile", profile)
          return
        end
      end
    end)
    if not ok then
      -- Leave WirePlumber's own choice standing rather than take the monitor
      -- down: a call on the earpiece is a nuisance, no sound at all is not.
      log:warning ("bluetooth call: keeping the profile failed - " .. tostring (err))
    end
  end,
}

keep_call_profile_hook:register ()
