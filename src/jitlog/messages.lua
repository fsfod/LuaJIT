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

local msgs = {
  {
    name = "header",
    "version : u32",
    "flags : u32",
    "headersize: u32",
    "msgtype_count :  u8",
    "msgsizes : i32[msgtype_count]",
    "msgnames_length : u32",
    "msgnames : stringlist[msgnames_length]",
    "cpumodel_length : u8",
    "cpumodel : string[cpumodel_length]",
    "os : string",
    "starttime : timestamp",
    "ggaddress : u64",
  },

  {
    name = "enumdef",
    "isbitflags : bool",
    "name : string",
    "namecount : u32",
    "valuenames_length : u32",
    "valuenames : stringlist[valuenames_length]",
  },

  {
    name = "stringmarker",
    "time : timestamp",
    "flags : 16",
    "label : string",
    use_msgsize = "label",
  },

  {
    name = "gcstring",
    "address : GCRef",
    "len : u32",
    "hash : u32",
    "data : string[len]",
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
    name = "gcfunc",
    "address : GCRefPtr",
    "proto_or_cfunc : ptr",
    "nupvalues : u8",
    "ffid : u8",
    { name = "upvalues", length = "nupvalues", type = "u64", argtype = "TValue *" },
  },

  {
    name = "gcproto",
    "address : GCRef",
    "chunkname : GCRef",
    "firstline : i32",
    "numline : i32",
    "bcaddr : MRef",
    "bclen : u32",
    "bc : u32[bclen]",
    "sizekgc : u32",
    "kgc : GCRef[sizekgc]",
    "lineinfosize : u32",
    "lineinfo : u8[lineinfosize]",
    "varinfo_size : u32",
    "varinfo : u8[varinfo_size]",
    structcopy = {
      fields = {
        "chunkname",
        "firstline",
        "numline",
        bclen = "sizebc",
        "sizekgc",
      },
      arg = "pt : GCproto *",
      store_address = "address",
    },
  },

  {
    name = "protoloaded",
    "time : timestamp",
    "address : GCRefPtr",
  },

  {
    name = "protobl",
    "time : timestamp",
    { name = "proto", type = "GCRefPtr", argtype = "GCproto *" },
    "bcindex : 24",
  },

  {
    name = "loadscript",
    "time : timestamp",
    "isloadstart : bool",
    "isfile : bool",
    "name : string",
    "mode : string",
  },

  {
    name = "scriptsrc",
    "length : u32",
    "sourcechunk : string[length]",
  },

  {
    name = "trace",
    "aborted : bool",
    "stitched : bool",
    "id : u16",
    { name = "parentid", type = "u16", argtype = "TraceNo1" },
    "startpt : GCRef",
    "stoppt : GCRefPtr",
    { name = "stoppc", type = "u32", argtype = "BCPos" },
    "stopfunc : GCRefPtr",
    "abortcode : u16",
    "nins : u16",
    "nk : u16",
    "root : u16",
    "nsnap: u16",
    "nsnapmap: u16",
    "spadjust : u16",
    "link : u16",
    { name = "startpc", type = "u32", argtype = "BCPos" },
    "mcodesize : u32",
    "mcodeaddr : ptr",
    "mcode_length : u32", -- Will be 0 unlike mcodesize when mcode data is filtered
    "mcode : u8[mcode_length]",
    "irlen : u32",
    { name = "ir", length = "irlen", type = "u64", argtype = "IRIns *" },
    "snapshots : u64[nsnap]",
    "snapmap : u32[nsnapmap]",

    structcopy = {
      fields = {
        id = "traceno",
        "startpt",
        "nins",
        "nk",
        "root",
        "nsnap",
        "nsnapmap",
        snapshots = "snap",
        "spadjust",
        "link",
        mcodeaddr = "mcode",
        "mcode",
        mcodesize = "szmcode",
        "snapmap",
      },
      arg = "trace : GCtrace *",
    },
  },

  {
    name = "traceexit_small",
    "isgcexit : bool",
    "traceid : 14",
    "exit : 9",
  },

  {
    name = "traceexit",
    "isgcexit : bool",
    "traceid : u16",
    "exit : u16",
  },

  {
    name = "exitstubs",
    "startid : u16",
    "first : ptr",
    "count : u16",
    "spacing : u16",
  },

  {
    name = "alltraceflush",
    "time : timestamp",
    "reason : u16",
    "tracelimit : u16",
    "mcodelimit : u32",
  },

  {
    name = "gcstate",
    "time : timestamp",
    "state : 8",
    "prevstate : 8",
    "totalmem : u32",
    "strnum : u32",
  },

  {
    name = "objlabel",
    "objtype : 4",
    "obj : GCRefPtr",
    "label : string",
    "flags : 20",
  },
}

local structs = {
}

return {messages = msgs, structs = structs}
