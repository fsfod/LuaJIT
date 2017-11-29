local format = string.format
local tinsert = table.insert

local builtin_types = {
  char = {size = 1, signed = true, printf = "%i", c = "char",  argtype = "char"},

  i8  = {size = 1, signed = true,  printf = "%i",   c = "int8_t",   argtype = "int32_t"},
  u8  = {size = 1, signed = false, printf = "%i",   c = "uint8_t",  argtype = "uint32_t"},
  i16 = {size = 2, signed = true,  printf = "%i",   c = "int16_t",  argtype = "int32_t"},
  u16 = {size = 2, signed = false, printf = "%i",   c = "uint16_t", argtype = "uint32_t"},
  i32 = {size = 4, signed = true,  printf = "%i",   c = "int32_t",  argtype = "int32_t"},
  u32 = {size = 4, signed = false, printf = "%u",   c = "uint32_t", argtype = "uint32_t"},
  i64 = {size = 8, signed = true,  printf = "%lli", c = "int64_t",  argtype = "int64_t"},
  u64 = {size = 8, signed = false, printf = "%llu", c = "uint64_t", argtype = "uint64_t"},

  MSize  = {size = 4, signed = false,  printf = "%u", c = "uint32_t", argtype = "MSize"},
  GCSize = {size = 4, signed = false,  printf = "%u", c = "GCSize", argtype = "GCSize"},

  timestamp  = {size = 8, signed = false,  printf = "%llu", c = "uint64_t", writer = "timestamp_highres", noarg = true},
  smallticks = {size = 4, signed = false,  printf = "%u",   c = "uint32_t", argtype = "uint64_t"},

  GCRef      = {size = 4, signed = false, c = "GCRef", printf = "0x%llx", writer = "setref", ref = "gcptr32", ref64 = "gcptr64", argtype = "GCRef"},
  --GCRef field with the value passed in as a pointer
  GCRefPtr   = {size = 4, signed = false, c = "GCRef", printf = "0x%llx", writer = "setref", ref = "gcptr32", ref64 = "gcptr64", ptrarg = true, argtype = "void *"},
  MRef       = {size = 4, signed = false, c = "MRef", printf = "0x%llx",  writer = "setref", ref = "ptr32", ref64 = "ptr64", ptrarg = true, argtype = "void *"},
  -- Always gets widen to 64 bit since this is assumed not to be a gc pointer
  ptr        = {size = 8, signed = false, c = "uint64_t", printf = "0x%llx", writer = "widenptr", ptrarg = true, argtype = "void *"},

  string     = {vsize = true, printf = "%s", string = true,     c = "const char*", argtype = "const char *",  element_type = "char", element_size = 1},
  stringlist = {vsize = true, printf = "%s", stringlist = true, c = "const char*", argtype = "const char *",  element_type = "char", element_size = 1},
}

local aliases = {
  int8_t  = "i8",  uint8_t  = "u8",
  int16_t = "i16", uint16_t = "u16",
  int32_t = "i32", uint32_t = "u32",
  int64_t = "i64", uint64_t = "u64",

  bitfield = "u32",
  bool = "u32",
}

for name, def in pairs(aliases) do
  assert(builtin_types[def])
  builtin_types[name] = builtin_types[def]
end

if GC64 then
  builtin_types.GCSize.size = 8

  builtin_types.GCRef.size = 8
  builtin_types.GCRef.ref = "gcptr64"
  builtin_types.GCRefPtr.size = 8
  builtin_types.GCRefPtr.ref = "gcptr64"

  builtin_types.MRef.size = 8
  builtin_types.MRef.ref = "ptr64"
end

local function make_arraytype(element_type)
  local element_typeinfo = builtin_types[element_type]
  local ctype = element_typeinfo.c or element_type
  local key = element_type.."[]"
  local typeinfo = {
    vsize = true,
    c = ctype.."*",
    argtype = format("const %s *", ctype),
    element_type = element_type,
    element_size = element_typeinfo.size,
  }
  builtin_types[key] = typeinfo
  builtin_types[ctype.."[]"] = typeinfo
  return typeinfo
end

make_arraytype("GCRef")

-- Build array types
for _, i in ipairs({1, 2, 4, 8}) do
  for _, sign in pairs({"i", "u"}) do
    make_arraytype(sign..(i*8))
  end
end

local function trim(s)
  return s:match("^%s*(.-)%s*$")
end

local function table_copyfields(src, dest, names)
  for _, k  in ipairs(names) do
    dest[k] = src[k]
  end
  return dest
end

local function map(t, f)
  local result = {}
  for _, v in ipairs(t) do
    table.insert(result, f(v))
  end
  return result
end

local parser = {
  verbose = true,
}

function parser:log(...)
  if self.verbose then
    print(...)
  end
end

local function parse_field(field)
  if type(field) == "table" then
    assert(field.name, "missing name on table field declaration")
    assert(field.type, "missing type on table field declaration")
    return field.name, field.type, field.argtype, field.length
  end

  local name, typename = string.match(field, "([^:]*):(.*)")
  name = trim(name or "")
  typename = trim(typename or "")
  if name == "" then
    error("invalid name in field " .. field)
  end
  if typename == "" then
    error("invalid type in field " .. field)
  end

  local arraytype, length = string.match(typename, "([^%[]*)%[([^%]]*)%]")
  if arraytype then
    typename = trim(arraytype)
    length = trim(length)
  end

  return name, typename, nil, length
end

local function parse_structcopy(msgdef, structcopy, fieldlookup)
  local arg_name, arg_type = parse_field(structcopy.arg)
  local struct_arg = arg_type..arg_name

  if structcopy.store_address then
    local struct_addr = fieldlookup[structcopy.store_address]
    assert(struct_addr, "store_address field '" .. structcopy.store_address .. "' does not exist")
    struct_addr.noarg = true
    struct_addr.struct_addr = true
    struct_addr.value_name = arg_name
  end

  -- The list of fields to copy is a mixed array and hashtable. Array entries mean we use the same field name for both the
  -- source struct and destination message field.
  for name, struct_field in pairs(structcopy.fields) do
    if type(name) == "number" then
      name = struct_field
    end
    local f = fieldlookup[name]
    if not f then
      error(format("No matching field for struct copy field '%s' in message '%s'", name, msgdef.name))
    end
    f.noarg = true
    f.struct_arg = arg_name
    f.struct_field = struct_field
  end

  return struct_arg
end

--[[
Field List
  noarg: Don't automatically generate an argument for the field in the generated logger function. Set for implict values like timestamp and string length
  ptrarg: The field value is passed as a pointer argument to the logger function
  bitsize: The number of bits this bitfield takes up
  bool: This field was declared as a boolean and we store it as bitfield with a bitsize of 1
  bitstorage: The name of the real field this bitfield is stuffed in most the time this will be some of the space 24 bits of the message id field thats always exists
  struct_arg: The name of the structure argument this field value will be copied from
  struct_field: The sub field from a structure arg that this field is assigned from
  struct_addr: contains the name of the struct argument whoes address is assigned to this field
  value_name: contains a varible name that will be will assigned to this field in the logger function
  buflen: The name of the argument or field that specifies the length of the array
  lengthof: The name of the field this field is providing an array length for
  vlen: This field is variable length blob of memory appended to the end of the message also set for strings
  vindex: Order of the field with respect to other variable length fields declared in message
  element_size: Size of elements in the varible length field in bytes. fieldesize = buflen * element_size
  implicitlen: The length of this field is implictly determined like for strings using strlen when they have no length arg
]]

function parser:parse_msg(def)
  assert(def[1], "message definition contains no fields")
  assert(def.name, "message definition has no name")

  local fieldlist = {}
  local fieldlookup = {}
  local vlen_fields = {}
  --Don't try to pack into the id if we have a base
  local offset = not def.base and 8 or 32
  local idsize = 8 -- bits used in the message id field 24 left
  local vcount = 0

  local function add_field(f, insert_index)
    local name = f.name
    assert(name, "no name specifed for field")
    if fieldlookup[name] then
      error("Duplicate field '"..name.."' in message "..def.name)
    end
    fieldlookup[name] = f
    if insert_index then
      table.insert(fieldlist, insert_index, f)
    else
      table.insert(fieldlist, f)
    end
  end

  add_field({name = "header", type = "u32", noarg = true, writer = "msghdr"})

  for _, field in ipairs(def) do
    local name, ftype, argtype, length = parse_field(field)
    local t = {name = name, type = ftype, argtype = argtype}
    local typeinfo = self.types[ftype]

    add_field(t)

    local bitsize = 32 -- size in bits

    if not typeinfo then
      t.type = "bitfield"
      bitsize = tonumber(ftype)
      assert(bitsize and bitsize < 32, "invalid bitfield size")
      t.bitsize = bitsize
    elseif ftype == "bool" then
      t.type = "bitfield"
      t.bitsize = 1
      t.bool = true
      bitsize = 1
    elseif typeinfo.vsize or length then
      vcount = vcount + 1
      vlen_fields[vcount] = t
      t.vindex = vcount
      t.vlen = true
      t.ptrarg = true
      t.buflen = length
      t.element_size = typeinfo.element_size or typeinfo.size
      -- Adjust the typename for primitive types to be an array
      if not typeinfo.element_size  then
        t.type = ftype.."[]"
        typeinfo = self.types[t.type]
        assert(typeinfo, "unknown buffer type")
      end

      -- If no length field name was specified make sure its one of the special cases it can be inferred
      if not length then
        if def.use_msgsize == t.name then
          t.buflen = "msgsize"
        elseif ftype == "string" then
          length = t.name.. "_length"
          assert(not fieldlookup[length], "cannot add automatic field length because the name is already taken")
          add_field({name = length, noarg = true, type = "uint32_t"})
          t.buflen = length
          t.implicitlen = true
        else
          error("Variable length field '"..t.name.."' did not specify a length field")
        end
      end
    else
      bitsize = self.types[ftype].size * 8
    end

    if bitsize < 32 then
       --Check if we can pack into the spare bits of the id field
      if idsize+bitsize <= 32 then
        t.bitstorage = "header"
        t.bitofs = idsize
        t.bitsize = bitsize
        idsize = idsize + bitsize
      elseif t.type == "bitfield" then
        offset = offset + bitsize
      end
    end
  end

  local struct_args = ""
  if def.structcopy then
    struct_args = parse_structcopy(def, def.structcopy, fieldlookup)
  end

  if vcount > 0 then
    assert(not def.use_msgsize or vcount == 1, "Can't use msgsize for field size with more then one variable sized field")

    for _, f in ipairs(vlen_fields) do
      if f.buflen and not def.use_msgsize then
        local buflen_field = fieldlookup[f.buflen]
        buflen_field.lengthof = f.name
        assert(buflen_field)
      end
    end

    --Add the implict message size field thats always after the message header
    add_field({name = "msgsize", sizefield = true, noarg = true, type = "u32", writer = "vtotal"}, 2)
  end

  local msgsize = 0
  for i, f in ipairs(fieldlist) do
    local size = 0
    local type = self.types[f.type]
    if f.type == "bitfield" or f.bitstorage then
      assert(f.bitstorage and fieldlookup[f.bitstorage])
    elseif f.vlen then
      assert(f.buflen)
      assert(fieldlookup[f.buflen])
    else
      assert(type, "unexpected type")
      assert(not f.bitsize)
      size = type.size
      assert(size == 1 or size == 2 or size == 4 or size == 8)
      f.offset = msgsize
    end
    f.order = i
    msgsize = msgsize + size
  end

  local result = {
    name = def.name,
    fields = fieldlist,
    vlen_fields = vlen_fields,
    fieldlookup = fieldlookup,
    size = msgsize,
    idsize = idsize,
    vsize = vcount > 0,
    vcount = vcount,
    sizefield = "msgsize",
    struct_args = struct_args,
  }
  return setmetatable(result, {__index = def})
end

function parser:parse_msglist(msgs)
  for _, def in ipairs(msgs) do
    local name = def.name
    self:log("Parsing:", name)
    local msg = self:parse_msg(def)
    self.msglookup[name] = msg
    table.insert(self.msglist, msg)
  end
end

parser.builtin_msgorder = {
  header = 0,
  enumdef = 1,
}

local function sortmsglist(msglist, msgorder)
  local names = map(msglist, function(def) return def.name end)
  msgorder = msgorder or {}

  -- Order the fixed built-in messages with a well know order
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
  return names
end

local copyfields = {
  "msglist",
  "msglookup",
  "sorted_msgnames",
  "types",
}

function parser:complete()
  assert(#self.msglist > 0)
  assert(self.msglookup["header"], "a header message must be defined")
  self.sorted_msgnames = sortmsglist(self.msglist, self.builtin_msgorder)

  local data = table_copyfields(self, {}, copyfields)
  return data
end

local generator = {}

local function joinlist_format(list, fmt, prefix, suffix, addmax)
  prefix = prefix or ""
  suffix = suffix or ""
  local t = {}
  for i, name in ipairs(list) do
    t[i] = format(fmt, name)
  end
  if addmax then
    table.insert(t, format(fmt, "MAX"))
  end
  return prefix .. table.concat(t, suffix .. prefix) .. suffix
end

local unescapes = {
  n = '\n',
  s = ' ',
  t = '\t',
}

local function unescape(s)
  return string.gsub(s, "\\(%a)", function(key)
    local result = unescapes[key]
    assert(result, "unknown escape")
    return result
  end)
end

function generator:buildtemplate(tmpl, values)
  return (string.gsub(tmpl, "{{(.-)}}", function(key)
    local name, fmt = string.match(key, "%s*(.-)%s*:(.+)")
    if name then
      key = name
      fmt = unescape(fmt)
    end

    local value = values[key]

    if value == nil then
      error("missing value for template key '"..key.."'")
    end

    if fmt then
      if type(value) == "table" then
        value = joinlist_format(value, fmt)
      else
        value = format(fmt, value)
      end
    end
    return value
  end))
end

function generator:write(s)
  self.outputfile:write(s)
end

function generator:writef(s, ...)
  assert(type(s) == "string" and s ~= "")
  self.outputfile:write(format(s, ...))
end

function generator:writetemplate(name, ...)
  local template = self.templates[name]
  local result
  if not template then
    error("Missing template "..name)
  end
  if type(template) == "string" then
    assert(template ~= "")
    result = self:buildtemplate(template, ...)
  else
    result = template(...)
  end

  self:write(result)
end

function generator:mkfield(f)
  local ret

  local comment_line = self.templates.struct_comment or self.templates.comment_line

  if f.type == "bitfield" then
    ret = format("\n/*  %s: %d;*/", f.name, f.bitsize)
  else
    local type = self.types[f.type]
    local langtype = type.c or f.type

    if f.vlen then
      langtype = type.element_type or langtype
      ret = "\n"..format(comment_line, format("%s %s[%s];", langtype, f.name, f.buflen))
    elseif f.bitstorage then
      ret = "\n"..format(comment_line, format("%s %s:%d", langtype, f.name, f.bitsize))
    else
      ret = format(self.templates.structfield, langtype, f.name)
    end
  end
  return ret
end

function generator:write_struct(name, def)
  local fieldstr = ""
  local fieldgetters = {}

  if def.base then
    local base = self.msglookup[def.base]
    assert(base, "Missing base message "..def.base)
    for _, f in ipairs(base.fields) do
      fieldstr = fieldstr..self:mkfield(f)
    end
  end

  for _, f in ipairs(def.fields) do
    fieldstr = fieldstr..self:mkfield(f)

    if self:needs_accessor(def, f) then
      local voffset
      if f.vindex and f.vindex > 1 then
        voffset = {}
        for i = 1, f.vindex-1 do
          local vfield = def.vlen_fields[i]
          local buflen = def.fieldlookup[vfield.buflen]
          voffset[i] = format("%s*%d", self:fmt_fieldget(def, buflen), vfield.element_size)
        end
        voffset = table.concat(voffset, " + ")
      end

      local getter = self:fmt_accessor_def(def, f, voffset)
      assert(getter)
      table.insert(fieldgetters, getter)
    end
  end

  self:writetemplate("struct", {name = name, fields = fieldstr, bitfields = fieldgetters})
  return #fieldgetters > 0 and fieldgetters
end

local function logfunc_getfieldvar(msgdef, f)
  local field = f.name
  if f.struct_arg then
    field = format("%s->%s", f.struct_arg, f.struct_field)
  elseif f.value_name then
    field = f.value_name
  end

  return field
end

function generator:write_vlenfield(msgdef, f, vtotal, vwrite)
  local tmpldata = {
    name = logfunc_getfieldvar(msgdef, f),
    sizename = f.name.."_size",
  }

  local vtype = self.types[f.type]
  tmpldata.element_size = vtype.element_size or vtype.size

  -- Check that the length is not an implicit arg after the field
  if f.buflen then
    tmpldata.sizename = f.buflen
    local szfield = msgdef.fieldlookup[f.buflen]
    if szfield then
      tmpldata.sizename = logfunc_getfieldvar(msgdef, szfield)
    end
  end

  if f.type == "string" and (f.implicitlen or msgdef.use_msgsize == f.name) then
    tinsert(vtotal, self:buildtemplate("MSize {{sizename}} = (MSize)strlen({{name}});", tmpldata))
  end
  tinsert(vtotal, self:buildtemplate("vtotal += {{sizename}} * {{element_size}};", tmpldata))
  tinsert(vwrite, self:buildtemplate("ubuf_putmem(ub, {{name}}, (MSize)({{sizename}} * {{element_size}}));", tmpldata))
end

local funcdef_fixed = [[
LJ_STATIC_ASSERT(sizeof(MSG_{{name}}) == {{msgsize}});

static LJ_AINLINE void log_{{name}}({{args}})
{
  MSG_{{name}} *msg = (MSG_{{name}} *)ubufP(ub);
{{fields:  %s\n}}  setubufP(ub, ubufP(ub) + {{msgsize}});
  ubuf_more(ub, {{minbuffspace}});
}

]]

local funcdef_vsize = [[
LJ_STATIC_ASSERT(sizeof(MSG_{{name}}) == {{msgsize}});

static LJ_AINLINE void log_{{name}}({{args}})
{
  MSG_{{name}} *msg;
{{vtotal:  %s\n}}  msg = (MSG_{{name}} *)ubuf_more(ub, (MSize)(vtotal + {{minbuffspace}}));
{{fields:  %s\n}}  setubufP(ub, ubufP(ub) + {{msgsize}});
{{vwrite:  %s\n}}
}

]]

generator.custom_field_writers = {
  timestamp_highres = "__rdtsc();",
  timestamp = "__rdtsc();",
  gettime = "__rdtsc();",
  setref = function(self, msgdef, f, valuestr)
    local setref = (f.type == "MRef" and "setmref") or "setgcrefp"
    local type = self.types[f.type]
    if f.ptrarg or type.ptrarg or f.struct_addr then
      return format("%s(msg->%s, %s);", setref, f.name, valuestr)
    else
      -- Just do an assignment for raw GCref values
      return format("msg->%s = %s;", f.name, valuestr)
    end
  end,
  msghdr = function(self, msgdef, f)
    return format("msg->header = MSGTYPE_%s;", msgdef.name)
  end,
  vtotal = "(uint32_t)vtotal;",
  widenptr = function(self, msgdef, f, valuestr)
    return format("msg->%s = (uint64_t)(uintptr_t)(%s);", f.name, valuestr)
  end,
}

function generator:write_logfunc(def)
  local fields = {}
  local vtotal, vwrite = {}, {}
  if def.vsize then
    tinsert(vtotal, format("size_t vtotal = sizeof(MSG_%s);", def.name))
  end

  local args = {"UserBuf *ub"}
  if def.struct_args ~= "" then
    table.insert(args, def.struct_args)
  end

  for _, f in ipairs(def.fields) do
    local typename = f.type
    local argtype
    local typedef = self.types[typename]

    if typename == "bitfield" then
      typename = "uint32_t"
    elseif typename == "string" then
      argtype = self.types[typename].argtype
    else
      typename = typedef.c
      argtype = f.argtype or typedef.argtype
    end

    -- Don't generate a function arg for fields that have implicit values. Also group arrays fields with
    -- their length field in the parameter list.
    if not typedef.noarg and not f.noarg and (not f.lengthof or def.fieldlookup[f.lengthof].noarg) then
      assert(not f.value_name and not f.struct_field)

      table.insert(args, format("%s %s", (argtype or typename), f.name))
      if f.buflen then
        local length = def.fieldlookup[f.buflen]
        if not length.noarg then
          table.insert(args, "uint32_t " .. length.name)
        end
      end
    end

    local field_assignment = f.name
    local writer = f.writer or typedef.writer

    if f.struct_arg then
      -- Field has it value set from a field inside struct passed in as a function argument
      field_assignment = format("%s->%s", f.struct_arg, f.struct_field)
    elseif f.value_name then
      field_assignment = f.value_name
    end

    if f.bitofs then
      field_assignment = format("(%s << %d)", field_assignment, f.bitofs)
    elseif not writer and argtype and typedef.size and typedef.size < 4 then
      -- truncate the value down to the fields size
      field_assignment = format("(%s)%s", typename, field_assignment)
    end

    if writer then
      local writerimpl = self.custom_field_writers[writer]
      assert(writerimpl, "missing writer")

      if type(writerimpl) == "function" then
        field_assignment = writerimpl(self, def, f, field_assignment)
      else
        field_assignment = format("msg->%s = %s", f.name, writerimpl)
      end
    elseif f.vlen then
      self:write_vlenfield(def, f, vtotal, vwrite)
      field_assignment = ""
    elseif f.bitstorage then
      -- Bit field is is stuffed in another field
      field_assignment = format("msg->%s |= %s;", f.bitstorage, field_assignment)
    else
      field_assignment = format("msg->%s = %s;", f.name, field_assignment)
    end

    if field_assignment ~= "" then
      table.insert(fields, field_assignment)
    end
  end

  local minbuffspace = "128"
  local template

  if #vtotal ~= 0 then
    template = funcdef_vsize
  else
    template = funcdef_fixed
  end

  args = table.concat(args, ", ")
  self:write(self:buildtemplate(template, {name = def.name, args = args, fields = fields, vtotal = vtotal, vwrite = vwrite, minbuffspace = minbuffspace, msgsize = def.size}))
end

function generator:build_boundscheck(msgdef)
  local checks = {}

  for _, field in ipairs(msgdef.vlen_fields) do
    local len = self:fmt_fieldget(msgdef,  msgdef.fieldlookup[field.buflen])
    table.insert(checks, self:buildtemplate(self.templates.boundscheck_line, {field = len, name = field.name, element_size = field.element_size}))
  end
  return self:buildtemplate(self.templates.boundscheck_func, {name = msgdef.name, msgsize = msgdef.size, checks = checks})
end

function generator:write_enum(name, names, prefix)
  prefix = prefix and (prefix .. "_") or name

  if self.outputlang ~= "c" then
    prefix = ""
  end
  local entries = joinlist_format(names, prefix..self.templates.enumline, "  ", "", true)
  self:writetemplate("enum", {name = name, list = entries})
end

function generator:write_namelist(name, names)
  self:writetemplate("namelist", {name = name, list = names, count = #names})
end

function generator:write_msgsizes(dispatch_table)
  local sizes = ""

  for _, name in ipairs(self.sorted_msgnames) do
    local size = self.msglookup[name].size
    if self.msglookup[name].vsize then
      if dispatch_table then
        size = 0
      else
        size = -size
      end
    end
    sizes = sizes .. format("  %d, %s\n", size, format(self.templates.comment_line, name))
  end

  local template

  if dispatch_table and self.templates.msgsize_dispatch then
    template = "msgsize_dispatch"
  else
    template = "msgsizes"
  end

  self:writetemplate(template, {list = sizes, count = #self.sorted_msgnames})
end

function generator:write_msgdefs()
  for _, def in ipairs(self.msglist) do
    self:write_struct(def.name, def)
  end
end

function generator:write_cheader(options)
  options = options or {}
  self.outputfile = io.open("lj_jitlog_def.h", "w")
  self:writefile(options)
  self.outputfile:close()

  self.outputfile = io.open("lj_jitlog_writers.h", "w")
  self:write_headerguard("timer_writers")
  self:write([[
#include "lj_jitlog_def.h"
#include "lj_usrbuf.h"

#if LJ_TARGET_LINUX || LJ_TARGET_OSX
#include <x86intrin.h>
#endif

]])
  for _, def in ipairs(self.msglist) do
    self:write_logfunc(def)
  end

  self:write("#endif\n")
  self.outputfile:close()

  -- Write the header for arrays that should only be in one translation unit
  self.outputfile = io.open("lj_jitlog_decl.h", "w")
  self:write_headerguard("jitlog_decl")
  self:write([[
#include "stdint.h"

]])
  self:write_namelist("jitlog_msgnames", self.sorted_msgnames)
  self:write_msgsizes()
  self:write_msgsizes(true)

  self:write("#endif\n")
  self.outputfile:close()
end

local lang_generator = {}

local function writelang(lang, data, options)
  options = options or {}

  local lgen = lang_generator[lang]
  if not lgen then
    lgen = require("jitlog."..lang.."_generator")
    lang_generator[lang] = lgen
  end

  local state = {}
  table_copyfields(data, state, copyfields)
  -- Allow the language generator to override base generator functions
  setmetatable(state, {
    __index = function(self, key)
      local v = lgen[key]
      return (v ~= nil and v) or generator[key]
    end
  })

  local filepath = options.filename or state.default_filename
  state.outputfile = io.open(filepath, "w")
  state:writefile(options)
  state.outputfile:close()
  return filepath
end

local c_generator = require("jitlog.c_generator")

local api = {
  create_parser = function()
    local t = {
      msglist = {},
      msglookup = {},
      types = setmetatable({}, {__index = builtin_types})
    }
    t.data = t
    return setmetatable(t, {__index = parser})
  end,

  writelang = writelang,
  write_c = function(data, options)
    local t = {}
    table_copyfields(data, t, copyfields)
    -- Allow the c generator to override base generator functions
    setmetatable(t, {
      __index = function(self, key)
        local v = c_generator[key]
        return (v ~= nil and v) or generator[key]
      end
    })

    t:write_cheader(options)
  end,
}

return api
