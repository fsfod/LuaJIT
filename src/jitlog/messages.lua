--[[
  The built-in numeric types are u8/s8, u16/s16, u32/s32, u64/s64 unsigned and signed respectively.
  Field types that are declared with just a number with no 's' or 'u' prefix are considered unsigned
  bit fields with a max size of 32 bits.
  Array fields are specified as a element type followed by open and close brackets with the name of
  the array's length field in-between them like this i32[elementcount_field].

Special field types
  ptr: A pointer field that is always stored as a 64 bit value on both 32 bit and 64 bit builds.
  GCRef: A pointer to a GC object passed in to the log function as a GCRef struct value. This type is
         32 bits wide on both 32 bit and 64 bit (non GC64) builds it grows to 64 bits for GC64 builds.
  GCRefPtr: The same as GCRef except the value is passed in as a pointer instead of a GCRef struct
            in the writer.
  bool: A boolean stored as a bit field with a bit width of one. It should be turned back into a native
        boolean value by the reader.
  timestamp: An implicit 64 bit time stamp generated when the message is written. By default this is
             the value returned from rdtsc.
  string: An array field that has extra logic in both the writer and reader side to work as a string.
          If no length field is specified one is implicitly generated and strlen is used to determine
          the length on the writer side and the null terminator skipped.
  stringlist: A list of strings joined together into an array but separated with nulls chars.

Message Flags:
  use_msgsize: If the message only has one variable length field, then omit this length field and use the
               message size field instead minus the fixed size part of the message.

]]--

local module = {}

module.messages = {
  {
    name = "header",
    fields = [[
      version : u32
      flags : u32
      headersize: u32
      msgtype_count :  u8
      msgsizes : i32[msgtype_count]
      msgnames_length : u32
      msgnames : stringlist[msgnames_length]
      cpumodel_length : u8
      cpumodel : string[cpumodel_length]
      os : string
      starttime : timestamp
      ggaddress : u64
    ]]
  },

  {
    name = "note",
    fields = [[
      time : timestamp
      isbinary : bool
      isinternal : bool
      label : string   
      data_size : u32
      data : u8[data_size] @argtype(const void*)
    ]]
  },

  {
    name = "enumdef",
    fields = [[
      isbitflags : bool
      name : string
      namecount : u32
      valuenames_length : u32
      valuenames : stringlist[valuenames_length]
    ]]
  },

  {
    name = "stringmarker",
    fields = [[
      time : timestamp
      jitted : bool
      flags : 16
      label : string
    ]],
    use_msgsize = "label",
  },

  {
    name = "idmarker4b",
    fields = [[
      jited : bool
      flags : 7
      id : 16
    ]]
  },

  {
    name = "idmarker",
    fields = [[
      time : timestamp
      jited : bool
      flags : 7
      id : 16
    ]]
  },

  {
    name = "obj_string",
    fields = [[
      address : GCRef
      len : u32
      hash : u32
      data : string[len]
    ]],
    structcopy = {
      fields = {
        "len",
        "hash",
      },
      arg = "s : GCstr *",
      store_address = "address",
    },
    use_msgsize = "len",
  },

  {
    name = "obj_proto",
    fields = [[
      address : GCRef
      size : u32
      chunkname : string
      firstline : i32
      numline : i32
      flags : 24
      numparams : u8
      framesize : u8
      uvcount : u8
      bcaddr : MRef
      bclen : u32
      bc : u32[bclen]
      sizeknum : u16
      knum : double[sizeknum]
      sizekgc : u16
      kgc : GCRef[sizekgc]
      lineinfosize : u32
      lineinfo : u8[lineinfosize]
      varnames_size : u32
      varnames : stringlist[varnames_size]
      varinfo_length : u32
      varinfo : VarRecord[varinfo_length]
      uvnames_size : u32
      uvnames : stringlist[uvnames_size]
    ]],
    structcopy = {
      fields = {
        "firstline",
        "numline",
        bclen = "sizebc",
        "sizekgc",
        sizeknum = "sizekn",
        "sizekgc",
        size = "sizept",
        "numparams",
        "framesize",
        uvcount = "sizeuv",
        "flags",
      },
      arg = "pt : GCproto *",
      store_address = "address",
    },
  },

  {
    name = "traceexit_small",
    fields = [[
      isgcexit : bool
      traceid : 14
      exit : 9
    ]]
  },

  {
    name = "traceexit",
    fields = [[
      isgcexit : bool
      traceid : u16
      exit : u16
    ]]
  },

  {
    name = "register_state",
    fields = [[
      source : u8
      gpr_count : u8
      fpr_count : u8
      vec_count : u8
      gprs_length : u16
      fprs_length : u16 
      vregs_length : u32
      gprs : u8[gprs_length]
      fprs : u8[fprs_length]
      vregs : u8[vregs_length]
    ]]
  },

  {
    name = "trace_flushall",
    fields = [[
      time : timestamp
      reason : u16 @enum(flushreason)
      tracelimit : u16
      mcodelimit : u32
    ]]
  },

  {
    name = "gcstate",
    fields = [[
      time : timestamp
      state : 8 @enum(gcstate)
      prevstate : 8
      totalmem : GCSize
      strnum : u32
    ]]
  },

  {
    name = "statechange",
    fields = [[
      time : timestamp
      system : 8
      state : 8
      flags : 8
    ]]
  },

  {
    name = "obj_label",
    fields = [[
      objtype : 4
      obj : GCRefPtr
      label : string
      flags : 20
    ]]
  },
}

module.structs = {
  {
    name = "VarRecord",
    fields = [[
      startpc : u32
      extent: u32
    ]]
  },
}

return module
