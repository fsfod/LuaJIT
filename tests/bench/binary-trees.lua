
local function BottomUpTree(item, depth)
  if depth > 0 then
    local i = item + item
    depth = depth - 1
    local left, right = BottomUpTree(i-1, depth), BottomUpTree(i, depth)
    return { item, left, right }
  else
    return { item }
  end
end

local function ItemCheck(tree)
  if tree[2] then
    return tree[1] + ItemCheck(tree[2]) - ItemCheck(tree[3])
  else
    return tree[1]
  end
end

local N = tonumber(arg and arg[1]) or 0
local mindepth = 4
local maxdepth = mindepth + 2
if maxdepth < N then maxdepth = N end

perfmarker("stretchtree:")
do
  local stretchdepth = maxdepth + 1
  local stretchtree = BottomUpTree(0, stretchdepth)
  collectgarbage()
  io.write(string.format("stretch tree of depth %d\t check: %d\n",
    stretchdepth, ItemCheck(stretchtree)))
  collectgarbage()
end

local longlivedtree = BottomUpTree(0, maxdepth)
collectgarbage()
for depth=mindepth,maxdepth,2 do
  local iterations = 2 ^ (maxdepth - depth + mindepth)
  local check = 0
  --for i=1,iterations do
    local tree1 = BottomUpTree(1, depth)
    local tree2 = BottomUpTree(-1, depth)
    collectgarbage()
    check = check + ItemCheck(tree1) + ItemCheck(tree2)
    tree1, tree2 = nil, nil
    collectgarbage()
 -- end
  
  io.write(string.format("%d\t trees of depth %d\t check: %d\n",
    iterations*2, depth, check))
end

io.write(string.format("long lived tree of depth %d\t check: %d\n",
  maxdepth, ItemCheck(longlivedtree)))
collectgarbage()
