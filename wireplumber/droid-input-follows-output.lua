-- Das Mikrofon soll der Ausgabewahl folgen.
--
-- Waehlt man einen Kopfhoerer als Ausgang, bleibt das Mikrofon sonst beim
-- Telefon: Ein- und Ausgang sind in PipeWire unabhaengig, und callaudiod nagelt
-- beide auf die Telefonkarte fest, sobald es sie erkennt
-- (SET_DEFAULT_SINK und SET_DEFAULT_SOURCE). Eine festgelegte Wahl wiegt
-- intern 30000 und schlaegt jede Prioritaet - das Kopfhoerermikrofon kaeme
-- also nie zum Zug, obwohl es mit 2010 hoeher steht als die 1000 des Telefons.
--
-- Diese Regel greift nur, wenn beide Enden zum selben Geraet gehoeren: waehlt
-- man den Kopfhoerer, wird auch dessen Mikrofon genommen. Fuer Geraete ohne
-- Mikrofon passiert nichts.

cutils = require ("common-utils")
log = Log.open_topic ("s-node")

function nodeByName (name)
  return cutils.get_object_manager ("node"):lookup {
    Constraint { "node.name", "=", name },
  }
end

function defaultMetadata ()
  return cutils.get_object_manager ("metadata"):lookup {
    Constraint { "metadata.name", "=", "default" },
  }
end

input_follows_output_hook = SimpleEventHook {
  name = "node/droid-input-follows-output",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "metadata-changed" },
      Constraint { "metadata.name", "=", "default" },
      Constraint { "event.subject.key", "=", "default.audio.sink" },
    },
  },
  execute = function (event)
    local props = event:get_properties ()
    local raw = props ["event.subject.value"]
    if not raw then
      return
    end

    local ok, parsed = pcall (function () return Json.Raw (raw):parse () end)
    local name = ok and parsed and parsed.name or nil
    if not name then
      return
    end

    local sink = nodeByName (name)
    local dev = sink and sink.properties ["device.id"]
    if not dev then
      log:debug ("Eingang folgt Ausgang: " .. name .. " gehoert zu keinem Geraet")
      return
    end

    local source = cutils.get_object_manager ("node"):lookup {
      Constraint { "media.class", "=", "Audio/Source" },
      Constraint { "device.id", "=", dev, type = "pw" },
    }
    if not source then
      log:debug ("Eingang folgt Ausgang: " .. name .. " hat kein Mikrofon")
      return
    end

    local src_name = source.properties ["node.name"]
    local m = defaultMetadata ()
    if not m then
      return
    end

    -- Nichts tun, wenn es ohnehin schon stimmt - sonst schaukelt sich das
    -- mit callaudiod gegenseitig hoch.
    local cur = m:find (0, "default.configured.audio.source")
    if cur and string.find (cur, src_name, 1, true) then
      return
    end

    log:info ("Eingang folgt Ausgang: " .. src_name .. " (wegen " .. name .. ")")
    m:set (0, "default.configured.audio.source", "Spa:String:JSON",
           Json.Object { name = src_name }:to_string ())
  end
}

input_follows_output_hook:register ()
