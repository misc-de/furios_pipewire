-- Droid-Monitor fuer WirePlumber.
--
-- Bindet das SPA-Device api.droid.device ein und macht aus dessen Objekten
-- Adapter-Knoten. Der Umweg ueber WirePlumber ist noetig: laesst man PipeWire
-- das Device direkt aus context.objects instanziieren, entstehen rohe
-- SPA-Knoten ohne Adapter - die haben keine Formatwandlung und erscheinen in
-- pipewire-pulse nicht als Sink oder Source.

cutils = require ("common-utils")
log = Log.open_topic ("s-monitors")

-- Routen ueber die Prozessgrenze reichen.
--
-- Das Device lebt hier in WirePlumber, die Knoten laufen im PipeWire-Daemon -
-- und nur der Knoten haelt den HAL-Stream, der umgeroutet werden muss. Der
-- Routenname geht deshalb als SPA_PROP_params-Paar an den Knoten; dort
-- uebersetzt das Plugin ihn in den devicePort des HAL.
-- Zuletzt zugestellte Route je Geraet: das Ereignis feuert auch, wenn sich an
-- der Route nichts geaendert hat - der HAL soll davon nichts merken.
last_route = {}

function forwardRoutes (dev)
  local nodes_om = cutils.get_object_manager ("node")

  for p in dev:iterate_params ("Route") do
    local route = cutils.parseParam (p, "Route")
    if route and route.device ~= nil and route.name then
      -- card.profile.device ist eine reine Knoteneigenschaft, keine globale -
      -- ohne type = "pw" findet der lookup nichts.
      local node = nil
      if last_route[route.device] ~= route.name then
        node = nodes_om:lookup {
          Constraint { "device.id", "=", tostring (dev["bound-id"]) },
          Constraint { "card.profile.device", "=", tostring (route.device), type = "pw" },
        }
      end
      if node then
        last_route[route.device] = route.name
        log:info ("droid: Route " .. route.name .. " an " ..
                     tostring (node.properties["node.name"]))
        node:set_param ("Props", Pod.Object {
          "Spa:Pod:Object:Param:Props", "Props",
          params = Pod.Struct { "droid.route", route.name }
        })
      end
    end
  end
end

-- Routenwechsel kommen als Ereignis, nicht als Signal am Proxy.
route_hook = SimpleEventHook {
  name = "monitor/droid-forward-routes",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Route" },
    },
  },
  execute = function (event)
    local dev = event:get_subject ()
    if dev.properties["device.api"] ~= "droid" then
      return
    end
    forwardRoutes (dev)
  end
}

function createNode (parent, id, obj_type, factory, properties)
  -- Pflichtfelder: ohne device.id gehoert der Knoten zu keiner Karte, ohne
  -- factory.name weiss der Adapter nicht, welchen SPA-Knoten er laden soll.
  properties["device.id"] = parent["bound-id"]
  properties["factory.name"] = factory
  properties["device.api"] = "droid"
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
    -- Der frisch angelegte Knoten kennt die aktive Route noch nicht.
    local dev = cutils.get_object_manager ("device"):lookup {
      Constraint { "device.api", "=", "droid" }
    }
    if dev then
      forwardRoutes (dev)
    end
  end)
end

device = SpaDevice ("api.droid.device", {
  ["device.name"] = "droid_card.primary",
  ["device.description"] = "Android HAL",
  ["device.nick"] = "droid",
  ["device.api"] = "droid",
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
route_hook:register ()

log:info ("droid: Device aktiviert")
