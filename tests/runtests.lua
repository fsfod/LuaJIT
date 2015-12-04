local tester = require("jit_tester")
local testjit = tester.testsingle
local telescope = require("telescope")

telescope.make_assertion("jit", "", function(expect, func, ...) 
  assert(func(...) == expect) 
  return true 
end)

telescope.make_assertion("jitchecker", "", function(checker, func, ...) 
  assert(checker(1, func(1, ...)) == nil) 
  return true 
end)

telescope.make_assertion("noexit", "", function(expect, func, ...)
  local result = func(...)
  assert(result == expect, tostring(result)) 
  return true 
end)

local callbacks = {}

function callbacks.before(t) 
  print("running", t.name) 
end

local contexts = {}
local files = {"intrinsic_spec.lua"}

for _, file in ipairs(files) do 
  telescope.load_contexts(file, contexts) 
end

local buffer = {}
local testfilter

if test_pattern then
  testfilter = function(t) return t.name:match(test_pattern) end
end

local results = telescope.run(contexts, callbacks, testfilter)
local summary, data = telescope.summary_report(contexts, results)
table.insert(buffer, summary)
local report = telescope.error_report(contexts, results)

if report then
  table.insert(buffer, "")
  table.insert(buffer, report)
end

if #buffer > 0 then 
  print(table.concat(buffer, "\n"))
end