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
    name = "stringmarker",
    "time : timestamp",
    "flags : 16",
    "label : string",
    use_msgsize = "label",
  },
}

return msgs
