-- SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
-- SPDX-License-Identifier: MIT
-- Checks and counting for the Lua tests.
local ROOT = os.getenv("TEST_ROOT") or "."
local base = dofile(ROOT .. "/tests/lua/run.lua")

local T = {
  wp = base.wp,
  root = base.root,
  load_script = base.load_script,
  traced = base.traced,
  checks = 0,
  failures = 0,
}

function T.suite(name)
  io.write(name, "\n")
end

function T.check(what, cond)
  T.checks = T.checks + 1
  if cond then
    io.write("  \27[32mok\27[0m   ", what, "\n")
  else
    T.failures = T.failures + 1
    io.write("  \27[31mFAIL\27[0m ", what, "\n")
  end
end

function T.check_equal(what, want, got)
  if want == got then
    T.check(what, true)
  else
    T.checks = T.checks + 1
    T.failures = T.failures + 1
    io.write("  \27[31mFAIL\27[0m ", what, ": expected ", tostring(want),
             ", got ", tostring(got), "\n")
  end
end

function T.done()
  io.write(string.format("\n  %d checks, %d failed\n", T.checks, T.failures))
  if os.getenv("LUA_COVERAGE") then
    T.report_coverage()
  end
  io.stdout:flush()
  os.exit(T.failures == 0 and 0 or 1, true)
end

function T.report_coverage()
  for name in pairs(base.covered) do
    local path = base.paths[name] or name
    local want = base.executable_lines(path)
    local total, hit, missing = 0, 0, {}
    for line in pairs(want) do
      total = total + 1
      if base.covered[name][line] then
        hit = hit + 1
      else
        table.insert(missing, line)
      end
    end
    table.sort(missing)
    io.write(string.format("  %-46s %5.1f%% of %d\n",
             name, total > 0 and hit / total * 100 or 100, total))
    if #missing > 0 and os.getenv("LUA_COVERAGE") == "lines" then
      io.write("      not reached: ", table.concat(missing, " ", 1, math.min(#missing, 25)), "\n")
    end
  end
end

return T
