local readerlib = require("jitlog.reader")
local format = string.format
  
local function parselog(path)
  local reader = readerlib.makereader(readerlib.mixins)
  reader:parsefile(path)
  return reader
end

local lib = {}

function lib.parselogs(paths, results)

  results = results or {}

  for i,path in ipairs(paths) do
    local log = parselog(path)
    table.insert(results, log)
  end

end

function lib.processmarkers(reader)

    local benchmarks = {}
    local currbench = ""
    local bench, prevaction
    
    for i, m in ipairs(reader.markers) do
    local prev = reader.markers[i - 1]
    local action, bench_name = string.match(m.label, "BENCH%(([^%)]+)%):%s*([a-zA-Z]+)")
    --print(action, bench_name)
    if not action then
        print(m.label)
    end
    
    if (bench_name and bench_name ~= currbench) or prevaction == "STOP" then
        assert(action == "LOAD" or prevaction == "STOP")
        currbench = bench_name
        bench = {name = bench_name, log = reader, load = false, start = false, stop = false}
        table.insert(benchmarks, bench)
    end
    
    if action == "LOAD" then
        bench.load = m
    elseif action == "START" then
        bench.start = m
    elseif action == "STOP" then
        bench.stop = m 
    end
    prevaction = action
    end 

    return benchmarks
end

local function print_diff(reader, label, m1, m2)
  if m2.traces - m1.traces + m2.aborts - m1.aborts + m2.flushes - m1.flushes > 0 then
    print(format("%s: traces %d, aborts %d, flushes %d, exits %d", label, m2.traces - m1.traces, m2.aborts - m1.aborts, m2.flushes - m1.flushes, 
                 m2.exits - m1.exits))
  end
  
  if m2.flushes - m1.flushes > 0 then
    for i= m1.flushes, m2.flushes do
      local flush = reader.flushes[i]
      print(format("FullTraceFlush(%s) maxtrace %d", flush.reason, flush.maxtrace))
    end
  end
  
  if m2.ptbl - m1.ptbl > 0 then
    for i= m1.ptbl+1, m2.ptbl do
      local bl = reader.proto_blacklist[i]
      print(format("ProtBlacklisted(%s:%d) opcode %s, bcindex %d", bl.proto.chunk, bl.proto:get_linenumber(bl.bcindex), bl.proto:get_bcop(bl.bcindex), bl.bcindex))
    end
  end
end

local log_benches = {}

for i, log in ipairs(logs) do
  log_benches[i] = processmarkers(log)
end

print("Benchmarks", #log_benches)

for i,benchmarks in ipairs(log_benches) do
  print("LOG", i, "----------------------------------------------------")
for i, bench in ipairs(benchmarks) do
  local start, stop = bench.start, bench.stop
  if bench.load then
    print_diff(bench.log, bench.name.."-LOAD", bench.load, bench.start)
  end
  
  print_diff(bench.log, bench.name.."-ITER"..i, bench.start, bench.stop)
end
end

