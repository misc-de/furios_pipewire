-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Enough of WirePlumber to run the scripts outside it.
--
-- The monitor scripts hold real decisions now - which sink a call goes to,
-- whether the microphone follows a headset, what a volume means - and twice
-- today an error in one of them took WirePlumber down and every sound with it.
-- That is the argument for testing them, and the obstacle: they only run
-- inside a session manager.
--
-- So this stands in for one. It is deliberately shallow: object managers
-- return what a test put there, params are plain tables, and every call the
-- script makes is recorded so the test can look at what it did rather than at
-- what it said. Nothing here pretends to be PipeWire - a script that passes
-- against this stub is a script whose logic holds, not one that is known to
-- work on the phone.

local M = {}

-- What the scripts did, for the test to inspect afterwards.
M.calls = {}

local function record(what, ...)
  table.insert(M.calls, { what = what, args = { ... } })
end

M.record = record

function M.calls_of(what)
  local out = {}
  for _, c in ipairs(M.calls) do
    if c.what == what then table.insert(out, c) end
  end
  return out
end

function M.reset()
  M.calls = {}
end

-- --- objects the scripts are handed ---------------------------------------

local Object = {}
Object.__index = Object

--- A node, device or metadata object as the scripts see it.
function M.object(props, extra)
  local o = setmetatable({}, Object)
  o.properties = props or {}
  o["bound-id"] = (props or {})["bound-id"] or 1
  o.params = (extra or {}).params or {}
  o.values = (extra or {}).values or {}
  return o
end

function Object:iterate_params(name)
  local list = self.params[name] or {}
  local i = 0
  return function ()
    i = i + 1
    return list[i]
  end
end

function Object:set_param(name, pod)
  record("set_param", self, name, pod)
end

function Object:set_params(name, pod)
  record("set_params", self, name, pod)
end

function Object:find(subject, key)
  return self.values[key]
end

function Object:set(subject, key, type_, value)
  record("metadata_set", key, type_, value)
  if value == nil then
    self.values[key] = nil
  else
    self.values[key] = value
  end
end

function Object:connect(signal, fn)
  record("connect", signal, fn)
end

function Object:activate(...)
  record("activate", ...)
end

function Object:call(method, ...)
  record("plugin_call", method, ...)
  if method == "get-volume" then
    return self.volume_answer
  end
  return nil
end

function Object:store_managed_object(id, obj)
  record("store_managed_object", id, obj)
end

function Object:set_managed_pending(id)
  record("set_managed_pending", id)
end

-- --- object managers -------------------------------------------------------
--
-- A lookup takes constraints; the stub matches them against the properties of
-- whatever the test registered. Only the two forms the scripts use are
-- supported: equality on a property, and equality on bound-id.

M.objects = { node = {}, device = {}, metadata = {} }

function M.add(kind, obj)
  table.insert(M.objects[kind], obj)
  return obj
end

local function matches(obj, constraints)
  for _, c in ipairs(constraints) do
    local key, op, want = c[1], c[2], c[3]
    local have
    if key == "bound-id" then
      have = obj["bound-id"]
    else
      have = obj.properties[key]
    end
    if op == "=" then
      if tostring(have) ~= tostring(want) then return false end
    end
  end
  return true
end

local Manager = {}
Manager.__index = Manager

function Manager:lookup(constraints)
  for _, obj in ipairs(M.objects[self.kind]) do
    if matches(obj, constraints) then return obj end
  end
  return nil
end

-- --- the globals the scripts expect ---------------------------------------

-- State the scripts keep between events. It lives in Lua globals, which
-- outlive a second dofile of the same script - so a test that loads a script
-- twice would otherwise start with the first run's memory of what it had
-- already sent, and send nothing.
local script_state = {
  "last_route", "last_route_volume", "last_mode", "last_volume",
  "in_bt_call", "saved_routes", "mixer", "device", "log",
  "gave_up", "defends",
}

function M.install()
  M.reset()
  M.objects = { node = {}, device = {}, metadata = {} }
  M.settings = {}
  for _, name in ipairs(script_state) do _G[name] = nil end

  local log = {}
  for _, level in ipairs({ "info", "debug", "warning", "notice", "message" }) do
    log[level] = function (_, msg) record("log", level, msg) end
  end
  Log = { open_topic = function () return log end }

  Constraint = function (t) return t end
  EventInterest = function (t) return t end

  M.hooks = {}
  SimpleEventHook = function (t)
    t.register = function (self) record("register", self.name); M.hooks[self.name] = self end
    return t
  end

  Pod = {
    Object = function (t) return { pod = "Object", body = t } end,
    Array  = function (t) return { pod = "Array", body = t } end,
    Struct = function (t) return { pod = "Struct", body = t } end,
  }

  Json = {
    Raw = function (str)
      return { parse = function () return M.json_parse(str) end }
    end,
    Object = function (t)
      return { to_string = function () return M.json_dump(t) end }
    end,
  }

  Feature = { SpaDevice = { ENABLED = 1 }, Proxy = { BOUND = 2 } }

  -- WirePlumber's settings. A script reading one that nobody set gets
  -- nothing back, which is what happens on a device where the schema entry is
  -- missing - and the scripts have to behave then too.
  M.settings = {}
  Settings = {
    get_boolean = function (key)
      record("settings_get", key)
      return M.settings[key]
    end,
  }

  Plugin = { find = function (name)
    record("plugin_find", name)
    return M.mixer
  end }

  SpaDevice = function (factory, props)
    record("spa_device", factory, props)
    if M.spa_device_fails then return nil end
    return M.add("device", M.object(props))
  end

  Node = function (factory, props)
    record("node", factory, props)
    return M.object(props)
  end

  local cutils = {
    get_object_manager = function (kind)
      return setmetatable({ kind = kind }, Manager)
    end,
    parseParam = function (pod, name)
      return M.parse_param(pod, name)
    end,
  }
  package.loaded["common-utils"] = cutils
  return cutils
end

-- A param as parseParam returns it: the tests hand in plain tables, so this is
-- the identity. Kept as a seam so a test can make parsing fail.
function M.parse_param(pod, name)
  if M.parse_fails then return nil end
  return pod
end

-- Just enough JSON for {"name":"..."}.
function M.json_parse(str)
  local name = str:match('"name"%s*:%s*"([^"]*)"')
  if name == nil then return nil end
  return { name = name }
end

function M.json_dump(t)
  return string.format('{ "name": "%s" }', tostring(t.name))
end

return M
