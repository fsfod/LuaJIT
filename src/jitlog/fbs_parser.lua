local tinsert = table.insert

local lex_word_start = {}
for c = string.byte("a"), string.byte("z") do
   lex_word_start[string.char(c)] = true
end
for c = string.byte("A"), string.byte("Z") do
   lex_word_start[string.char(c)] = true
end
lex_word_start["_"] = true

local lex_word = {}
for c = string.byte("a"), string.byte("z") do
   lex_word[string.char(c)] = true
end
for c = string.byte("A"), string.byte("Z") do
   lex_word[string.char(c)] = true
end
for c = string.byte("0"), string.byte("9") do
   lex_word[string.char(c)] = true
end
lex_word["_"] = true

local lex_decimal_start = {}
for c = string.byte("0"), string.byte("9") do
   lex_decimal_start[string.char(c)] = true
end

local lex_decimals = {}
for c = string.byte("0"), string.byte("9") do
   lex_decimals[string.char(c)] = true
end

local lex_hexadecimals = {}
for c = string.byte("0"), string.byte("9") do
   lex_hexadecimals[string.char(c)] = true
end
for c = string.byte("a"), string.byte("f") do
   lex_hexadecimals[string.char(c)] = true
end
for c = string.byte("A"), string.byte("F") do
   lex_hexadecimals[string.char(c)] = true
end

local lex_char_symbols = {}
for _, c in ipairs({"[", "]", "(", ")", "{", "}", ",", "#", "`", ";", ":", "@", "*", "="}) do
   lex_char_symbols[c] = "seperator"
end

local lex_op_start = {}
for _, c in ipairs({ "+", "*", "/", "|", "&", "%", "^", }) do
   lex_op_start[c] = true
end

local lex_space = {}
for _, c in ipairs({ " ", "\t", "\v", "\n", "\r", }) do
   lex_space[c] = true
end

local lexer = {}

local x, y, i
local i, limit
local input = ""
local peek = ""
local newline_start, line_start
local lines

function lexer.init(inputstr)
  input = inputstr
  limit = #input
  x, y, i = 1, 1, 0
  line_start = 1
  lines = {1}
  peek = input:sub(1, 1)
end

local function nextchar()
  if i == limit then
    peek = nil
    return false, x, y
  elseif i == newline_start then
    assert(peek ~= "\n" or peek ~= "\r")
    y = y + 1
    x = 1
    newline_start = nil
    line_start = i +1
    lines[#lines] = line_start
  else
    x = x + 1
  end

  i = i + 1
  local c = peek

  if i+1 <= limit then
    peek = input:sub(i+1, i+1)
  else
    peek = false
  end

  if not newline_start then
    if c == '\n' then
      newline_start = i
    elseif c =='\r' then
      if peek == '\n' then
        newline_start = i + 1
      else
        newline_start = i
      end
    end
  end

  return c, y, x
end

local function save_location(loc, length)
  loc = loc or {}
  loc.start = i
  loc.length = length or 0
  loc.line = y
  loc.col = x

  return loc
end

local function copy_location(loc, dest)
  dest.start = loc.start
  dest.length = loc.length
  dest.line = loc.line
  dest.col = loc.col

  return dest
end

local function peekchar()
  if i ~= newline_start then
    return peek, y, x+1
  else
    return peek, y+1, 1
  end
end

local function lex_tileol()
  local c = nextchar()

  while c and c ~= '\n' and c ~= '\r'  do
    c = nextchar()
  end
  local lineend = i
  if c == '\r' and peek == '\n' then
    nextchar()
  end
  return lineend
end

local function lex_error(loc, msg)
  assert(loc.line)
  assert(msg)
  local errmsg = string.format("LexError: %s at line %d col %d\n", msg, loc.line, loc.col)
  local lineend = string.find(input, "[\n\r]", line_start)
  errmsg = errmsg..input:sub(line_start, lineend or #input)
  error(errmsg)
end

local function lex_identifier(loc)
  nextchar()
  local start = i
  save_location(loc)

  while peek and lex_word[peek] do
    local c = nextchar()
  end
  local length = i-start + 1
  assert(length ~= 0)
  loc.length = length

  local identifier = input:sub(start, i)

  if identifier == "true" or identifier == "false" then
    return "boolean", identifier == "true", loc
  else
    return "identifier", identifier, loc
  end
end

local function lex_string(loc)
  local endchar = nextchar()
  save_location(loc)
  local start, lastchunk = i, i+1
  assert(endchar == '"')
  local c = nextchar()
  local s

  while c and c ~= endchar do
    if c == '\\' then
      s = (s or "") .. input:sub(lastchunk, i)
      lastchunk = i+1
    end

    if c == '\n' or c == '\n' then
      loc.length = i-start
      lex_error(loc, "Unexpected line end before closing \" in string")
      break
    end
    c = nextchar()
  end

  loc.length = i-start

  if not c then
    lex_error(loc, "Reached end of file while parsing string")
  end

  if not s then
    s = input:sub(start+1, i-1)
  end

  return s, loc
end

local function lex_number(loc)
  local c = nextchar()
  local start = i
  save_location(loc)

  while peek and lex_decimals[peek] do
    nextchar()
  end
  loc.length = i-start

  local number = input:sub(start, i)
  local success, value = pcall(tonumber, number)

  if not success or not value then
    lex_error(loc, "Failed to parse number "..number.." "..(value or ""))
    value = 0
  end

  return value, loc
end

local temploc = {}

function lexer.lex_token(loc)
  local endi
  loc = loc or temploc

  while true do
    local c, line, col = peekchar()

    if not c then
      return false
    end

    if lex_word_start[c] then
      return lex_identifier(loc)
    elseif c == '"' then
      return "string", lex_string(loc)
    elseif lex_decimal_start[c] then
      return "number", lex_number(loc)
    elseif lex_char_symbols[c] then
      nextchar()
      save_location(loc, 1)
      return "token", c, loc
    elseif c == '/' then
      nextchar()
      if peek == '/' then
        endi = lex_tileol()
      else
        save_location(loc, 1)
        return "token", '/', loc
      end
    elseif c == '-' then
      nextchar()
      if peek == '-' then
        endi = lex_tileol()
      else
        save_location(loc, 1)
        return "token", '-', loc
      end
    elseif not lex_space[c] then
      save_location(loc)
      lex_error(loc, "Unexpected character " ..c)
    else
      nextchar()
    end
  end
end

function lexer.dumptokens()
  while true do
    local kind, value, loc =  lexer.lex_token()
    if kind then
      print(kind, value, loc.line, loc.col)
    else
      break
    end
  end
end

local tokencache =  {{type = false}, {type = false}}
local curtoken, peektoken

local parser = {}

function parser.init(inputstr)
  lexer.init(inputstr)
  curtoken, peektoken = tokencache[1], tokencache[2]

  curtoken.type, curtoken.value = lexer.lex_token(curtoken)
  peektoken.type, peektoken.value = lexer.lex_token(peektoken)
end

local function nexttoken()
  if not peektoken then
    curtoken = false
    return false
  end

  local token = peektoken
  local type, value = lexer.lex_token(curtoken)
  -- check if this is the last token in the file
  if type then
    curtoken.type, curtoken.value = type, value
    peektoken = curtoken
  else
    peektoken = nil
  end

  curtoken = token
  return token
end
parser.nexttoken = nexttoken

function parser.error(loc, msg, ...)
  local errmsg = "ParseError: "..string.format(msg, ...)
  errmsg = errmsg..string.format(" at line %d col %d\n", loc.line, loc.col)

  local lineend = string.find(input, "[\n\r]", line_start)
  errmsg = errmsg..input:sub(line_start, lineend or #input)
  error(errmsg)
end

local function eat_token(kind)
  if curtoken.type == "token" and curtoken.value == kind then
    nexttoken()
    return true
  else
    parser.error(curtoken, "Expected token %s but found %s", kind, curtoken.value)
  end
end

local function tryeat_token(kind)
  if curtoken and curtoken.type == "token" and curtoken.value == kind then
    nexttoken()
    return true
  else
    return false
  end
end

local function eat_identifier(loc)
  if curtoken and curtoken.type == "identifier" then
    local identifier = curtoken.value
    if loc then
      copy_location(curtoken, loc)
    end
    nexttoken()
    return identifier
  else
    parser.error(curtoken, "Expected identifier but found %s", curtoken and curtoken.type or "EOF")
  end
end

local function tryeat_identifier()
  if curtoken.type == "identifier" then
    local identifier = curtoken.value
    nexttoken()
    return identifier
  end
end

local function eat_number()
  if curtoken.type == "number" then
    local identifier = curtoken.value
    nexttoken()
    return identifier
  else
    parser.error(curtoken, "Expected number but found %s", curtoken.type)
  end
end

local function eat_keyword(keywords)
  if curtoken.type == "identifier" and keywords[curtoken.value] then
    local identifier = curtoken.value
    nexttoken()
    return identifier
  else
    local list = ""
    for k, _ in pairs(keywords) do
      list = list .. k .. " or "
    end
    list = list:sub(1, -5)
    parser.error(curtoken, "Expected %s but found %s", list, curtoken.type)
  end
end

local function curtok_issep(kind)
  if curtoken and curtoken.type == "token" then
    return curtoken.value == kind
  else
    return false
  end
end

local function peek_seperator()
  if peektoken.type == "token" then
    return peektoken.value
  end
end

local function peek_token()
  return peektoken.type, peektoken.value, peektoken
end

local value_tokens = {
  string = true,
  boolean = true,
  number = true,
}

local function curtok_isvalue()
  if value_tokens[curtoken.type] then
    return true, curtoken.value
  else
    return false, nil
  end
end

local function eat_value()
  if value_tokens[curtoken.type] then
    assert(curtoken.value ~= nil)
    return curtoken.value
  else
    parser.error(curtoken, "Expected string\number\bool but found %s", curtoken.type)
  end
end

local function parse_fbattribs()
  eat_token('(')

  local attributes = {}
  local hascomma = false
  while true do
    if not hascomma and tryeat_token(')') then
      break
    end
    hascomma = false

    local attr = {}
    local name = eat_identifier(attr)
    local value
    if tryeat_token(':') then
      -- Default to the value parsed from the token to make less work for the generator
      if curtoken.type == "string" or curtoken.type == "number" or curtoken.type == "identifier" then
        value = curtoken.value
      end

      -- If there is more then one token for the value turn it into a string based on the start of
      -- the first token and the end of the last token thats.
      local start, endi = curtoken.start, curtoken.start + curtoken.length
      while true do
        local type, tok, loc = peek_token()
        if type == "token" and (tok == "," or tok == ")") then
          break
        end
        value = nil
        endi = loc.start + loc.length
        nexttoken()
      end

      nexttoken()

      if value == nil then
        value = input:sub(start, endi-1)
      end
    end

    attr.name = name
    attr.value = value
    attributes[#attributes+1] = attr
    if value ~= nil then
      attributes[name] = value
    else
      attributes[name] = true
    end

    if tryeat_token(',') then
      hascomma = true
    end
  end
  return attributes
end

local parse_luatable

local function parse_luatable_value()
  local isvalue, value = curtok_isvalue()
  if isvalue then
    nexttoken()
    return value
  elseif curtok_issep('{') then
    return parse_luatable()
  else
    parser.error(curtoken, "Expected a string, number, boolean or table for table value")
  end
end

local function parse_luatable_entry()
  local key

  if curtoken.type == "identifier" or (curtoken.type == "number" and peek_seperator() == '=') then
    key = curtoken.value
    nexttoken()
    eat_token('=')
    return key, parse_luatable_value()
  elseif tryeat_token('[') then
    if curtoken.type ~= "string" and curtoken.type ~= "number" then
      parser.error(curtoken, "Expected a string, number table key")
    end
    key = curtoken.value
    nexttoken()
    eat_token(']')
    eat_token('=')
    return key, parse_luatable_value()
  else
    return nil, parse_luatable_value()
  end
end

function parse_luatable()
  local t = {}
  local count = 0

  eat_token('{')
  while true do
    if curtok_issep('}') then
      break
    end
    local key, value = parse_luatable_entry()

    if key then
      -- store in hash table part of the table
      t[key] = value
    else
      count = count + 1
      t[count] = value
    end

    if not tryeat_token(',') then
      -- Breakout of the loop if this is the end of the table or its incomplete
      break
    end
  end
  eat_token('}')
  return t
end

local function parse_fieldlist()
  local fields, extra = {}, {}
  local count = 0

  while not curtok_issep('}')  do
    local f = {}
    local name = eat_identifier(f)
    f.name = name

    if tryeat_token('=') then
      extra[name] = parse_luatable_value()
    else
      eat_token(':')
      f.isarray = tryeat_token('[')

      local type
      if f.isarray then
        type = tryeat_identifier()

        if tryeat_token(':') then
          f.size = eat_number()
        end

        eat_token(']')
      else
        type = tryeat_identifier()

        if not type and not f.isarray then
          -- Convert bitfield size in to a string like the way the generator uses it
          type = eat_number()..""
        end
      end
      f.type = type

      -- Parse a default value if one is provided
      if tryeat_token('=') then
        f.value = eat_value()
      end

      if curtok_issep('(') then
        f.attributes = parse_fbattribs()
      end

      tryeat_token(';')

      count = count+1
      fields[count] = f
    end
  end

  return fields, extra
end

local function parse_enumlist()
  local fields = {}
  local count = 0

  while not curtok_issep('}') do
    local f = {}
    f.name = eat_identifier(f)

    if tryeat_token('=') then
      f.value = eat_number()
    end

    if curtok_issep('(') then
      f.attribute = parse_fbattribs()
    end

    tryeat_token(',')

    count = count+1
    fields[count] = f
  end
  return fields
end

local function parse_rpc_methods()
  local funcs = {}
  local count = 0

  while true do
    local f = {}
    f.loc = {}
    f.name = eat_identifier(f)

    eat_token('(')
    f.arg = eat_identifier()
    eat_token(')')

    eat_token(':')

    f.ret = eat_identifier()

    if curtok_issep('(') then
      f.attribute = parse_fbattribs()
    end
    
    tryeat_token(';')

    count = count+1
    funcs[count] = f

    if curtok_issep('}') then
      break
    end
  end
  return funcs
end

local toplevel = {
  message = true,
  struct = true,
  table = true,
  enum = true,
  rpc_service = true
}

function parser.parse_fbs(inputstr, verbose)
  parser.init(inputstr)
  local schema = {
    messages = {},
    structs = {},
    tables = {},
    enums = {},
    rpcs = {},
  }

  while curtoken do
    local object = {}
    copy_location(curtoken, object)
    local kind = eat_keyword(toplevel)
    object.kind = kind
    object.name = eat_identifier()

    if verbose then
      print("Parsing:", object.kind, object.name)
    end

    if kind == "enum" and tryeat_token(':') then
      object.basetype = eat_identifier()
    end

    if curtok_issep('(') then
      object.attributes = parse_fbattribs()
    else
      object.attributes = {}
    end

    eat_token('{')
    if kind == "enum" then
      object.values = parse_enumlist()
    elseif kind == "rpc_service" then
      object.methods = parse_rpc_methods()
    else
      object.fields, object.extra = parse_fieldlist()
    end
    eat_token('}')

    if kind == "message" then
      tinsert(schema.messages, object)
    elseif kind == "struct" then
      tinsert(schema.structs, object)
    elseif kind == "table" then
      tinsert(schema.tables, object)
    elseif kind == "enum" then
      tinsert(schema.enums, object)
    elseif kind == "rpc_service" then
      tinsert(schema.rpcs, object)
    end
  end

  return schema
end

local lib = {
  parser =  parser,
  lexer = lexer,
}

function lib.parse_fbsfile(path)
  local file = assert(io.open(path, "r"))
  local text = file:read("*all")
  file:close()

  local fbs = parser.parse_fbs(text)
  fbs.path = path
  fbs.text = text
  return fbs
end

function lib.parse_fbsstring(text)
  assert(type(text) == "string")
  assert(#text > 0)
  return parser.parse_fbs(text)
end

return lib
