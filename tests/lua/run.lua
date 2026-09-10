-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Runs the WirePlumber scripts against the stub, and counts the lines each one
-- reaches.
--
-- Coverage comes from Lua's own line hook rather than a tool, for the same
-- reason the rest of the suite has no framework: this has to run on the phone.
local ROOT = os.getenv("TEST_ROOT") or "."
local wp = dofile(ROOT .. "/tests/lua/wireplumber-stub.lua")

local covered = {}      -- file -> set of executed lines
local watching = {}     -- file name -> true
local paths = {}        -- file name -> full path

-- Keyed by file name, not by path: Lua truncates a long short_src, so an
-- absolute path stops matching what load_script registered - and the coverage
-- silently reads as nothing rather than failing.
local function basename(path)
  return (path:gsub(".*/", ""))
end

local function hook(_, line)
  local src = basename(debug.getinfo(2, "S").short_src)
  if watching[src] then
    covered[src] = covered[src] or {}
    covered[src][line] = true
  end
end

--- Load a monitor script under the stub, with its lines counted.
local function load_script(path)
  watching[basename(path)] = true
  paths[basename(path)] = path
  local chunk = assert(loadfile(path))
  debug.sethook(hook, "l")
  local okay, err = pcall(chunk)
  debug.sethook()
  if not okay then
    error("loading " .. path .. " failed: " .. tostring(err))
  end
end

--- Run something with the line hook on, so calls into the script count too.
local function traced(fn)
  debug.sethook(hook, "l")
  local okay, err = pcall(fn)
  debug.sethook()
  if not okay then error(err) end
end

-- Lines a Lua chunk can actually reach: gcov counts what the compiler emits,
-- and this is the nearest honest equivalent - every line with code on it.
local function executable_lines(path)
  local lines = {}
  local n = 0
  for line in io.lines(path) do
    n = n + 1
    local trimmed = line:match("^%s*(.-)%s*$")
    -- Skipped, because Lua's line hook never reports them: block ends, and
    -- the header line of a function, whose body carries the coverage. Leaving
    -- them in would put a floor under every file that no test could lift.
    local is_end = trimmed == "end" or trimmed == "}" or trimmed == "})"
                   or trimmed == "else" or trimmed == "end)" or trimmed == "end,"
                   or trimmed == "then" or trimmed == "do"
    local is_function_header = trimmed:match("function%s*%(") ~= nil
                               or trimmed:match("^function%s") ~= nil
    if trimmed ~= "" and not trimmed:match("^%-%-") and not is_end
       and not is_function_header then
      lines[n] = true
    end
  end
  return lines
end

return {
  wp = wp,
  paths = paths,
  root = ROOT,
  load_script = load_script,
  traced = traced,
  executable_lines = executable_lines,
  covered = covered,
}
