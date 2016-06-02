local format = string.format
local msgs, outputfile
msglist = msglist or {}
msglookup = msglookup or {}

local getticksstr = "__rdtsc()"

local numtypes = {
  int8_t  = {size = 8,   signed = true,   printf = "%i", ctype = "int8_t",   cstype = "sbyte", argtype = "int32_t"},
  uint8_t = {size = 8,   signed = false,  printf = "%i", ctype = "uint8_t",  cstype = "byte",  argtype = "uint32_t"},
  
  int16_t  = {size = 16, signed = true,   printf = "%i", ctype = "int16_t",  cstype = "short",  argtype = "int32_t"},
  uint16_t = {size = 16, signed = false,  printf = "%i", ctype = "uint16_t", cstype = "ushort", argtype = "uint32_t"},
  
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
  timestamp = {size = 64, signed = false,  printf = "%ull", ctype = "uint64_t", cstype = "ulong", customwrite = true, noarg = true},
  smallticks = {size = 32, signed = false,  printf = "%u", ctype = "uint32_t", cstype = "uint", argtype = "uint64_t"},

  GCRef = {size = 32, signed = false, ctype = "GCRef", printf = "%p", ref = "gcptr32"},
  MRef =  {size = 32, signed = false, ctype = "MRef", printf = "%p", ref = "ptr32"},
  ptr =  {size = 32, signed = false, ctype = "void*", ref = true},

  string = {vsize = true, signed = false, printf = "%s", ctype = "void*", string = true, vsize = true, ctype = "uint64_t", cstype = "string",  argtype = "const char *", customwrite = true},
}

for name, def in pairs(numtypes) do
  if type(def) == "string" then
    numtypes[name] = numtypes[def]
  end
end

function buildtemplate(tmpl, values)
  return string.gsub(tmpl, "({{[^\n]-}})", function(key)
    key = key:sub(3, -3)
    assert(values[key] ~= nil, "missing value for template key")
    return values[key]
  end)
end

local function joinlist(list, prefix, suffix)
  prefix = prefix or ""
  suffix = suffix or ""
  return prefix .. table.concat(list, suffix .. prefix) .. suffix
end

function write(s)
  print(s)
  if outputfile then
    outputfile:write(s)
  end
end

local function writef(s, ...)
  assert(type(s) == "string" and s ~= "")
  write(format(s, ...))
end

local function writetemplate(s, ...)
  assert(type(s) == "string" and s ~= "")
  write(buildtemplate(s, ...))
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
  local vsize, sizefield = false, nil
  
  for i, field in ipairs(def) do
    local name, type = parse_field(field)
    local t = {name = name, type = type, order = i}
    table.insert(fieldlist, t)

    local size = 32 -- size in bits

    if not numtypes[type] then
      t.type = "bitfield"
      size = tonumber(type)
      assert(size or size < 32, "invalid bitfield size")
      t.bitsize = size
    elseif numtypes[type].vsize then
      vsize = true
    else
      size = numtypes[type].size
    end

    if name == def.sizefield then
      t.sizefield = true
      t.noarg = true
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

  --If message has barible sized fields and theres no field declared to specify the messages variable size
  if vsize and not def.sizefield then
    sizefield = "msgsize"
    table.insert(fieldlist, 1, {name = "msgsize", type = "u32", order = 0})
    msgsize = msgsize + 4
  end

  local result = {name = msgname, fields = fieldlist, size = msgsize, idsize = idsize, vsize = vsize, sizefield = sizefield}
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

local function mkbitget(f, fieldref)
  return format("((%s >> %d) & 0x%x)", fieldref, f.bitofs, bit.lshift(1, f.bitsize)-1)
end

function mkfield(f)
  local ret, getter

  if f.type == "bitfield" then
    ret = format("\n/*  %s: %d;*/", f.name, f.bitsize)
  else
    local type = numtypes[f.type]
    if f.bitofs or f.idpart or f.type == "string" then
      ret = format("/* %s %s;*/", type.ctype, f.name)
    else
      ret = format("\n  %s %s;", type.ctype, f.name)
    end
  end
  return ret
end

local structdef = [[
typedef struct MSG_{{name}}{
  uint32_t msgid;{{fields}}
} MSG_{{name}};

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
    if f.type == "bitfield" then
      writef("#define %smsg_%s(msg) %s\n", name, f.name, mkbitget(f, "(msg)->msgid"))
    end
  end
  
  writetemplate(structdef, {name = name, fields = fieldstr})
end

local funcdef = [[
static LJ_AINLINE void log_{{name}}({{args}})
{
  SBuf *sb = &eventbuf;
  MSG_{{name}} *msg = (MSG_{{name}} *)sbufP(sb);
{{vtotal}}  {{fields}}  setsbufP(sb, sbufP(sb)+sizeof(MSG_{{name}}));{{vwrite}}
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
  local vtotal, vwrite, args = "", "", ""
    
  for i, f in ipairs(def.fields) do
    local type = f.type
    local argtype
    local typedef = numtypes[type]

    if type == "bitfield" then
      type = "uint32_t"
    elseif type == "string" then
      argtype = numtypes[type].argtype
    else
      type = typedef.ctype
      argtype = typedef.argtype and numtypes[typedef.argtype].ctype
    end

    --time stamp values are fetched inside the logger func so skip creating an arg for them
    if not typedef.noarg and not f.noarg then
      args = args .. format("%s%s %s", args ~= "" and ", " or "", argtype or type, f.name)
    end
    
    local field = f.name
    if f.sizefield then
      field = "vtotal"
    end

    if f.bitofs or f.idpart then
      field = format("(%s << %d)", field, f.bitofs)
    elseif not typedef.customwrite and argtype then
      field = format("(%s)%s", type, field)
    end

    if typedef.customwrite then
      if f.type == "timestamp" then
        field = format("  msg->%s = %s;\n", f.name, getticksstr)
      elseif f.type == "string" then
        if vtotal == "" then
          vtotal = format("  MSize vtotal = sizeof(MSG_%s);\n", def.name)
        end
        vtotal = vtotal .. buildtemplate("  MSize {{name}}_size = (MSize)strlen({{name}})+1;\n  vtotal += {{name}}_size;\n", {name = f.name})

        vwrite = vwrite .. format("\n  lj_buf_putmem(sb, %s, %s_size);\n", f.name, f.name)
        field = ""
      end
    elseif f.idpart then
      field = format("  msg->msgid |= %s;\n", field)
    else
      field = format("  msg->%s = %s;\n", f.name, field)
    end
    fieldstr = fieldstr .. field
  end
  
  if vtotal then
    
  end

  writetemplate(funcdef, {name = def.name, args = args, fields = fieldstr, vtotal = vtotal, vwrite = vwrite})
end

local printdef = [[
static LJ_AINLINE MSize print_{{name}}(void* msgptr)
{
  MSG_{{name}} *msg = (MSG_{{name}} *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_{{name}});
  printf("{{fmtstr}}\n", {{args}});
  return {{msgsz}};
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

  local msgsz, args = def.size + 4, ""
  local fmtlist = {}
    
  for i, f in ipairs(def.fields) do
    local type = numtypes[f.type]
    local fmtspec = type.printf
    local arg 

    if f.bitofs or f.idpart then
      arg = mkbitget(f, "msg->msgid")
    end

    --translate known enum ids into a name
    if f.name == "id" and def.enumlist then
      arg = format("%s_names[%s]", def.enumlist, arg)
      fmtspec = "%s"
    elseif arg then
      --arg = format("(msg->msgid >> %d) & %d", f.bitofs, bit.rshift(1, f.bitsize)-1)
    elseif f.type == "string" then
      arg = "(const char*)(msg+1)"
    elseif type.ref then
      arg = format("(uintptr_t)msg->%s.%s", f.name, type.ref)
    else
      arg = format("msg->%s", f.name)
    end

    if def.vsize and f.name == def.sizefield then
      msgsz = arg
    end

    local comma = (i ~= #def.fields and ", ")

    fmtlist[#fmtlist+1] = format("%s %s", f.name, fmtspec)

    if i ~= #def.fields then 
      arg = arg ..", "
    end
    args = args .. arg
  end

  local fmtstr = format('%s: %s', def.name, table.concat(fmtlist, ", "))
  writetemplate(printdef, {name = def.name, fmtstr = fmtstr, args = args, msgsz = msgsz})
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
  "lj_gc.c",
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
%s  %sMAX
};

]]

local luaenumdef = [[
local %s = {
%s
  %sMAX
}

]]

function write_enum(name, names, prefix)
  prefix = prefix and (prefix .. "_") or name
  local entries = joinlist(names, "  " .. prefix, ",\n")
  local enum = format(enumdef, name, entries, prefix);
  write(enum)
end

local namedef = [[
static const char *%s[] = {
%s};

]]

function write_namelist(name, names)
  local names = joinlist(names, '  "', '",\n')
  local enum = format(namedef, name, names);
  write(enum)
end

local namescans = {
  timers = {
    patten = "TimerStart%(([^%,)]+)", 
    enumname = "TimerId",
    enumprefix = "Timer",
  },

  sections = {
    patten = "Section_Start%(([^%,)]+)", 
    enumname = "SectionId",
    enumprefix = "Section",
  }
}

function gathernames()

  for name, def in pairs(namescans) do
    local t = getnames(def.patten)
    --assert(#t > 0)
    def.matches = t
  end

end

gathernames()

msgs = {
  time = {
    "id : 16",
    "time : u32",
    "flags : 8",
    enumlist = "timers",
  },

  section = {
    "id : 23",
    "time : timestamp",
    "start : 1",
    enumlist = "sections",
  },

  marker = {
    "id : 16",
    "flags : 8",
  },

  gcstate = {
    "state : 8",
    "prevstate : 8",
    "totalmem : u32",
    "time : timestamp",
  },

  arenacreated = {
    "address : MRef",
    "id : u32",
    "flags : 16",
  },

  arenasweep = {
    "arenaid : 24",
    "time : u32",
    "sweeped : u16",
    "celltop : u16",
  },

  gcobj = {
    "kind : 4",
    "size : 20",
    "address : GCRef",
  },

  stringmarker = {
    "flags : 16",
    "size : u32",
    "label : string",
    "time : timestamp",
    sizefield = "size",
  },
--[[
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

local msgorder = {
  marker = 1,
  time = 2,
  section = 3,
}

local names = map(msglist, function(def) return def.name end)

--Order fixed builtin messages before the 
table.sort(names, function(a, b) 
  if not msgorder[a] and not msgorder[b] then
   return a < b
  else  
   if msgorder[a] and msgorder[b] then
     return msgorder[a] < msgorder[b]
   else
    return msgorder[a] ~= nil
   end
  end
end)

write([[#include <stdio.h>

#pragma pack(push, 1)

]])

write_enum("MSGIDS", names, "MSGID")

write("static uint32_t msgsizes[] = {\n")
for _, name in ipairs(names) do
  local size = msglookup[name].size + 4
  if msglookup[name].vsize then
    size = 0
  end
  write(format("  %d, /* %s */\n", size, name))
end
write("};\n\n")

for key, def in ipairs(msglist) do

  if def.enumlist then
    local names = namescans[def.enumlist]
    write_enum(names.enumname, names.matches, names.enumprefix)
    write_namelist(def.enumlist.."_names", names.matches)
  end

  write_struct(def.name, def)
  write_logfunc(def)
  write_printfunc(def)
end

write([[
typedef MSize (*msgprinter)(void* msg);

static msgprinter msgprinters[] = {
]])

write(joinlist(names, "  print_", ",\n"))
write("};\n\n")

write("#pragma pack(pop)")

outputfile:close()

return


