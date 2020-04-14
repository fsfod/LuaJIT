
jitlog.start()
	Starts the JITLog if its not currently started with a memory buffer as the storage backend
	
jitlog.shutdown()
	Shutdowns the JITLog and removes its hooks

jitlog.reset()
	Resets the contents of the JITLog
	
jitlog.reset_memorization", jlib_reset_memorization},

jitlog.setresetpoint() save the current position in the JITLog to reset to with reset_tosavepoint
jitlog.reset_tosavepoint()

jitlog.save(filepath : string)
	Saves the JITLog to a file path specified
	
string jitlog.savetostring() 
	Saves a snapshot of the full JITLog to a Lua string
	
number jitlog.getsize()
 returns the current size of the JITLog in bytes

jitlog.setlogsink(path) 
	set the JITLog storage backend to a memory mapped file and copies the existing data to it
	
jitlog.writemarker(message : string, [flags : int])
	Writes a timestamped string marker to the JITLog with the user specified message and optional flags
	
jitlog.setmode(mode : string, enable : bool)
jitlog.getmode", jlib_getmode}
jitlog.labelobj", jlib_labelobj}
jitlog.labelproto", jlib_labelproto},
jitlog.write_stacksnapshot", jlib_write_stacksnapshot},

jitlog.memorize_existing()
	Memorize\writes currently allocated Lua objects to the JITLog. Types of objects written can be filter to only some types like Lua function protos

jitlog.write_perfcounts", jlib_write_perfcounts},
jitlog.write_perftimers", jlib_write_perftimers},

jitlog.section_start(name)
	Writes the start of performance section to the JITLog
jitlog.section_end", jlib_section_end},

jitlog.write_rawobj(obj : object, extramem: bool)
	Writes a raw memory snapshot of a Lua object to the JITLog and optionally its extra contents like the array and hash part of a Lua table

jitlog.write_gcsnapshot", jlib_write_gcsnapshot},
jitlog.setgcstats_enabled", jlib_setgcstats_enabled},
jitlog.write_gcstats", jlib_write_gcstats},
jitlog.reset_gcstats", jlib_reset_gcstats},
jitlog.set_objalloc_logging", jlib_set_objalloc_logging},


Compile time define LUAJIT_JITLOG_AUTOSTART to auto start the JITLog for each Lua state created

The LuaJIT binary supports a new environment variable LUA_JITLOG that auto starts the JITLog and optionally sets its file save path. This mode can also be enabled for the LuaJIT library itself 
with the compile time define LUAJIT_JITLOG_ENVSTART.
