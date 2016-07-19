t= {}
print("loopstart")
perfmarker("LoopStart")
for i=1,20000000 do
  t[i] = 1ll+i
end
perfmarker("LoopEnd")
collectgarbage()
collectgarbage("stop")

for i=1,20000000 do
  assert(t[i] == 1ll+i)
end

t = nil
perfmarker("Swept dead table")
collectgarbage()