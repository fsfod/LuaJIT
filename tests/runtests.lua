local tester = require("jit_tester")
local testjit = tester.testsingle
local telescope = require("telescope")

telescope.make_assertion("jit", "", tester.testsingle)
telescope.make_assertion("jitchecker", "", tester.testwithchecker)
telescope.make_assertion("noexit", "", tester.testnoexit)

filter = filter or ""

local function printfail()
  print("  Failed!")
end

local callbacks = {}

callbacks.err = printfail
callbacks.fail = printfail


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

if filter then

  if(type(filter) == "table") then  
    testfilter = function() 
      for _,patten in ipairs(filter) do
        if t.name:match(filter) then
          return true
        end
      end
      
      return false
    end
  elseif(filter ~= "") then 
    testfilter = function(t) return t.name:match(filter) end
  end
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