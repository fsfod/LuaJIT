local format = string.format
local msgs, outputfile
msglist = msglist or {}
msglookup = msglookup or {}

local numtypes = {
  int8_t  = {size = 8,   signed = true,   printf = "%i", ctype = "int8_t",   cstype = "sbyte"},
  uint8_t = {size = 8,   signed = false,  printf = "%i", ctype = "uint8_t",  cstype = "byte"},
  
  int16_t  = {size = 16, signed = true,   printf = "%i", ctype = "int16_t",  cstype = "short"},
  uint16_t = {size = 16, signed = false,  printf = "%i", ctype = "uint16_t", cstype = "ushort"},
  
  int32_t  = {size = 32, signed = true,   printf = "%i", ctype = "int32_t",  cstype = "int"},
  uint32_t = {size = 32, signed = false,  printf = "%u", ctype = "uint32_t", cstype = "uint"},
  
  int64_t  = {size = 64, signed = true,   printf = "%ill", ctype = "int64_t",  cstype = "long"},
  uint64_t = {size = 64, signed = false,  printf = "%ull", ctype = "uint64_t", cstype = "ulong"},

  i8  = "int8_t",  u8  = "uint8_t",
  i16 = "int16_t", u16 = "uint16_t",
  i32 = "int32_t", u32 = "uint32_t",
  i64 = "int64_t", u64 = "uint64_t",
  MSize = "uint32_t",
  bitfield = "uint32_t",

  GCRef = {size = 32, signed = false, ctype = "GCRef", printf = "%p", ref = "gcptr32"},
  MRef =  {size = 32, signed = false, ctype = "MRef", printf = "%p", ref = "ptr32"},
  ptr =  {size = 32, signed = false, ctype = "void*", ref = true},
}

for name, def in pairs(numtypes) do
  if type(def) == "string" then
    numtypes[name] = numtypes[def]
  end
end

function write(s)
  print(s)
  if outputfile then
    outputfile:write(s)
  end
end

local function trim(s)
  return s:match("^%s*(.-)%s*$")
end

function map(t, f)
  local result = {}
  for _, v in ipairs(t) do
    table.insert(result, f(v))
  end
  return result
end

local function parse_field(field)
  local name, type = string.match(field, "([^:]*):(.*)")
  name = trim(name or "")
  type = trim(type or "")
  if name == "" then
    error("invalid name in field " .. field)
  end
  if type == "" then
    error("invalid type in field " .. field)
  end
  return name, type
end

function parse_msg(msgname, def)
  assert(def[1], "message definition contains no fields")

  local fieldlist = {}
  --Don't try to pack into the id if we have a base
  local offset = not def.base and 8 or 32
  local msgsize = 0
  local idsize = 8 -- bits used in the message id field 24 left
  
  for i, field in ipairs(def) do
    local name, type = parse_field(field)
    local t = {name = name, type = type}
    table.insert(fieldlist, t)

    local size = 32 -- size in bits

    if not numtypes[type] then
      t.type = "bitfield"
      size = tonumber(type)
      assert(size or size < 32, "invalid bitfield size")
      t.bitsize = size
    else
      size = numtypes[type].size
    end

    if size < 32 then
       --Check if we can pack into the spare bits of the id field
      if (idsize+size) <= 32 then
        t.idpart = true
        t.bitofs = idsize
        t.bitsize = size
        idsize = idsize + size
      elseif t.type == "bitfield" then
        offset = offset + size
      else
        msgsize = msgsize + (size/8)
      end
    else
      msgsize = msgsize + (size/8)
      offset = 0
    end
  end

  local result = {name = msgname, fields = fieldlist, size = msgsize, idsize = idsize}
  return setmetatable(result, {__index = def})
end

function parse_msglist()
  for msgname, def in pairs(msgs) do
    print("Parsing:", msgname)
    local msg = parse_msg(msgname, def)
    msglookup[msgname] = msg
    table.insert(msglist, msg)
  end
end

function mkfield(f)
  if f.type == "bitfield" then
    return format("\n/*  %s: %d;*/", f.name, f.bitsize)
  else
    local type = numtypes[f.type]
    if f.bitofs or f.idpart then
      return format("/* %s %s;*/", type.ctype, f.name)
    else
      return format("\n  %s %s;", type.ctype, f.name)
    end
  end
end

local structdef = [[
typedef struct MSG_%s{
  uint32_t msgid;%s
} MSG_%s;

]]

function write_struct(name, def)

  local fieldstr = ""

  if def.base then
    local base = msglookup[def.base]
    assert(base, "Missing base message "..def.base)
    for i, f in ipairs(base.fields) do   
      fieldstr = fieldstr..mkfield(f)
    end
  end

  for i, f in ipairs(def.fields) do   
    fieldstr = fieldstr..mkfield(f)
  end
  
  write(format(structdef, name, fieldstr, name))
end

local funcdef = [[
static LJ_AINLINE void log_%s(%s)
{
  SBuf *sb = &eventbuf;
  MSG_%s *msg = (MSG_%s *)sbufP(sb);
  %s  setsbufP(sb, sbufP(sb)+sizeof(MSG_%s));
  lj_buf_more(sb, 16);
}

]]

function write_logfunc(def)
  local msgid = def.msgid

  if not msgid then
    local base = def.base and msglookup[def.base]
    assert(not def.base or base, "missing base message base")
    if base then
      msgid = base.msgid or base.name
    else
      msgid = def.name
    end
  end

  local fieldstr = format("msg->msgid = MSGID_%s;\n", msgid)
  local args = ""
    
  for i, f in ipairs(def.fields) do
    local type = f.type

    if type == "bitfield" then
      type = "uint32_t"
    else
      type = numtypes[type].ctype
    end

    args = args .. format("%s %s%s", type, f.name, i ~= #def.fields and ", " or "")
    
    local field = f.name
    if f.bitofs or f.idpart then
      field = format("(%s << %d)", f.name, f.bitofs)
    end

    if f.idpart then
      field = format("  msg->msgid |= %s;\n", field)
    else
      field = format("  msg->%s = %s;\n", f.name, field)
    end
    fieldstr = fieldstr .. field
  end
  
  write(format(funcdef, def.name, args, def.name, def.name, fieldstr, def.name))
end

local printdef = [[
static LJ_AINLINE void print_%s(void* msgptr)
{
  MSG_%s *msg = (MSG_%s *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_%s);
  printf("%s\n", %s);
}

]]

function write_printfunc(def)
  local msgid = def.msgid

  if not msgid then
    local base = def.base and msglookup[def.base]
    assert(not def.base or base, "missing base message base")
    if base then
      msgid = base.msgid or base.name
    else
      msgid = def.name
    end
  end

  local args = ""
  local fmtlist = {}
    
  for i, f in ipairs(def.fields) do
    local type = numtypes[f.type]
    local arg 

    if f.bitofs or f.idpart then
      arg = format("((%s >> %d) & 0x%x)", "msg->msgid", f.bitofs, bit.lshift(1, f.bitsize)-1)
    end

    if arg then
      --arg = format("(msg->msgid >> %d) & %d", f.bitofs, bit.rshift(1, f.bitsize)-1)
    elseif type.ref then
      arg = format("(uintptr_t)msg->%s.%s", f.name, type.ref)
    else
      arg = format("msg->%s", f.name)
    end

    local comma = (i ~= #def.fields and ", ")

    fmtlist[#fmtlist+1] = format("%s %s", f.name, type.printf)

    if i ~= #def.fields then 
      arg = arg ..", "
    end
    args = args .. arg
  end

  local fmtstr = format('%s: %s', def.name, table.concat(fmtlist, ", "))
  write(format(printdef, def.name, def.name, def.name, def.name, fmtstr, args))
end

local filecache = {}

local function readfile(path)
  if filecache[path] then
    return filecache[path]
  end
  local f = io.open(path, "rb")
  assert(f, "failed to open "..path)
  local content = f:read("*all")
  f:close()
  filecache[path] = content
  return content
end


function file_getnames(path, patten, t, seen)
  local file = readfile(path)
  seen = seen or {}
  t = t or {}
  
  for name in string.gmatch(file, patten) do
    name = trim(name)
    if not seen[name] then
      table.insert(t, name)
      seen[name] = true
    end
  end
  return t, seen
end

local paths = {
  "lj_gc2.c",
  "lj_gcarena.c",
}

function getnames(patten)
  local t, seen
  
  for _, path in ipairs(paths) do
    t, seen = file_getnames(path, patten, t, seen)
  end

  table.sort(t)
  return t
end

local enumdef = [[
enum %s{
  %s,
  %sMAX
};

]]

function write_enum(name, names, prefix)
  prefix = prefix and (prefix .. "_") or name
  local entries = prefix .. table.concat(names, ",\n  " .. prefix)
  local enum = format(enumdef, name, entries, prefix);
  write(enum)
  write_namelist(name.."_namelist", names)
end

local namedef = [[
static const char *%s[] = {
  "%s"
};

]]

function write_namelist(name, names)
  local names = table.concat(names, '",\n  "')
  local enum = format(namedef, name, names);
  write(enum)
end

local namescans = {
  Timers = {
    patten = "TimerStart%(([^%,)]+)", 
    enumname = "TimerIds",
    enumprefix = "Timer",
  },

  Sections = {
    patten = "Section_Start%(([^%,)]+)", 
    enumname = "SectionId",
    enumprefix = "Section",
  }
}

function gathernames()

  for name, def in pairs(namescans) do
    local t = getnames(def.patten)
    assert(#t > 0)
    def.matches = t
  end

end

gathernames()

msgs = {
  time = {
    "id : 16",
    "time : u32",
    "flags : 8",
    enumlist = "Timers",
  },

  section = {
    "id : 23",
    "time : uint64_t",
    "start : 1",
    enumlist = "Sections",
  },

  marker = {
    "id : 16",
    "flags : 8",
  },

  arenacreated = {
    "address : MRef",
    "id : u32",
    "flags : 16",
  },

--[[
  arenasweep = {
    "arenaid : u32",
    "time : u32",
    "sweeped : u16",
    "celltop : u16",
  },

  gcobj = {
    "kind : 4",
    "size : 20",
    "address : GCRef",
  },

  trace = {
    "id : u16",
    "parentid : u16",
    "pt : GCRef",
    "nins : u16",
    "nks : u16",
    "szmcode : u32",
    "rootid : u16",
    "nsnap : u16",   
    "spadjust : u16",
    "link : u16",
    "startpc : u32",
    "time : uint64_t",
    base = "gcobj",
    structcopy = true,
  },]]
}

parse_msglist()

outputfile = io.open("timerdef.h", "w")

local names = map(msglist, function(def) return def.name end)
table.sort(names)
write_enum("MSGIDS", names, "MSGID")

write("static uint32_t msgsizes[] = {\n")
for _, name in ipairs(names) do
  local size = msglookup[name].size + 4
  write(format("  %d, /* %s */\n", size, name))
end
write("};\n\n")

for key, def in ipairs(msglist) do

  if def.enumlist then
    local names = namescans[def.enumlist]
    write_enum(names.enumname, names.matches, names.enumprefix)
  end

  write_struct(def.name, def)
  write_logfunc(def)
  write_printfunc(def)
end

outputfile:close()

return


