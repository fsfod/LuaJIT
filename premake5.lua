
local flaglist = {
    --"LUA_USE_ASSERT",
    --"LUA_USE_APICHECK",
    "LUA_BUILD_AS_DLL",
    --"LUAJIT_NUMMODE", --1 all number are stored doubles, 2 dual number mode
    --"LUAJIT_ENABLE_LUA52COMPAT",
    --"LUAJIT_ENABLE_CHECKHOOK", -- check if any Lua hook is set while in jitted code
    --"LUAJIT_USE_SYSMALLOC", 
    
    --"LUAJIT_ENABLE_TABLE_BUMP",
    --"LUAJIT_TRACE_STITCHING",
    
    --"LUAJIT_DISABLE_JIT",
    --"LUAJIT_DISABLE_FFI",
    --"LUAJIT_DISABLE_VMEVENT",
    --"LUAJIT_DISABLE_DEBUGINFO",
    
    --"LUAJIT_DEBUG_RA",
    --"LUAJIT_CTYPE_CHECK_ANCHOR",
    --"LUAJIT_USE_GDBJIT",
    --"LUAJIT_USE_PERFTOOLS",
}

function BuildDynasmFlags(buildflags)
    
    local flags = ""
    
    if buildflags["LUAJIT_ENABLE_LUA52COMPAT"] then
        flags = flags.." -D LJ_52"
    end
    
    
    if buildflags["LUAJIT_NUMMODE"] == 2 then
        flags = flags.." -D DUALNUM"
    end
end

liblist = {
    "lib_base.c",
    "lib_math.c",
    "lib_bit.c",
    "lib_string.c", 
    "lib_table.c",
    "lib_io.c",
    "lib_os.c",
    "lib_package.c",
    "lib_debug.c",
    "lib_jit.c",
    "lib_ffi.c",
}

liblistString = "%{sln.location}src/"..table.concat(liblist, " %{sln.location}src/")

--local libs = os.matchfiles("src/lib_*.c")

function BuildVmCommand(cmd, outputfile, addLibList, outputDir)
    
    outputDir = outputDir or "$(IntDir)"
    
    local result =  '"../obj/buildvm/%{cfg.buildcfg}/%{cfg.platform}/buildvm.exe" '..cmd..' -o "'..outputDir..outputfile..'" '
    
    if addLibList then
        result = result..liblistString
    end
    
    return result
end




-- A solution contains projects, and defines the available configurations
solution "LuaJit"
   configurations { "Debug", "Release" }
   platforms { "x32", "x64" }
   defines {"_CRT_SECURE_NO_DEPRECATE" }
   objdir "obj/%{prj.name}/%{cfg.buildcfg}/%{cfg.platform}/"
   targetdir "bin/%{cfg.buildcfg}/%{cfg.platform}"
   startproject"lua"
   
   project "MiniLua"
      uuid "74FBF227-E0DA-71C3-E9F2-FC995551D824"
      kind "ConsoleApp"
      configurations { "Release" }
      language "C"
      location "build"
      targetdir "obj/%{prj.name}/%{cfg.buildcfg}/%{cfg.platform}/"
      vpaths { ["Sources"] = "src/host" }
      files {
        "src/host/minilua.c", 
      }

      configuration "Debug"
         defines { "NDEBUG" }
         optimize"Speed"
 
      configuration "Release"
         defines { "NDEBUG" }
         optimize"Speed" 
    

   -- A project defines one build target
   project "buildvm"
      uuid "B86F1F94-244F-9E2F-2D67-290699C50491"
      kind "ConsoleApp"
      dependson { "miniLua" } 
      vectorextensions "SSE2"
      location "build"
      language "C"
      targetdir "obj/%{prj.name}/%{cfg.buildcfg}/%{cfg.platform}/"
      files {
        "src/vm_x86.dasc",
        "src/host/buildvm*.c",
      }
      includedirs{
        "%{cfg.objdir}",
        "src"
      }

      filter{'architecture:x32', 'files:src/vm_x86.dasc'}
        buildmessage 'Compiling %{file.relpath}'
        buildcommands {
           '"../obj/minilua/%{cfg.buildcfg}/%{cfg.platform}/minilua.exe" %{sln.location}dynasm/dynasm.lua -LN -D WIN -D JIT -D FFI -o %{cfg.objdir}buildvm_arch.h %{file.relpath}'
        }
        buildoutputs { '%{cfg.objdir}/buildvm_arch.h' }
        
      --needed for adding -D P64 for 64 bit builds
      filter{'architecture:x64', 'files:src/vm_x86.dasc'}
        buildmessage 'Compiling %{file.relpath}'
        buildcommands {
           '"../obj/minilua/%{cfg.buildcfg}/%{cfg.platform}/minilua.exe" %{sln.location}dynasm/dynasm.lua -LN -D WIN -D JIT -D FFI -D P64 -o %{cfg.objdir}buildvm_arch.h %{file.relpath}'
        }
        buildoutputs { '%{cfg.objdir}/buildvm_arch.h' }


      configuration  {"Debug"}
         optimize"Speed"
 
      configuration {"Release"}
         optimize"Speed"
 
   -- A project defines one build target
   project "lua"
      uuid "C78D880B-3397-887C-BC12-9F7C281B947C"
      kind "SharedLib"
      buildoptions "/c"
      dependson { "buildvm", "miniLua"} 
      targetname "lua51"
      vectorextensions "SSE2"
      defines(flaglist)
      language "C++"
      location "build"
      vpaths { ["libs"] = "src/lib_*.h" }
      vpaths { ["libs"] = "src/lib_*.c" }
      vpaths { ["headers"] = "src/lj_*.h" }
      vpaths { [""] = "lua.natvis" }
      
      includedirs{
        "%{cfg.objdir}",
        "src"
      }
      
      files {
        "src/lj_*.h",
        "src/lj_*.c",
        "src/lib_*.h",
        "src/lib_*.c",
        "lua.natvis",
        
        --'$(IntDir)lj_vm.obj',--obj/lua/%{cfg.buildcfg}/%{cfg.platform}/
        
        '$(IntDir)/lj_bcdef.h',
        '$(IntDir)/lj_ffdef.h',
        '$(IntDir)/lj_libdef.h',
        '$(IntDir)/lj_recdef.h',
        '$(IntDir)/lj_folddef.h',
      }
      excludes
      {
        "src/*_arm*",
        "src/*_mips*",
        "src/*_ppc*",
      }
      
      linkoptions {'"$(IntDir)lj_vm.obj"'}
      
      prebuildcommands {
        '{MKDIR} %{cfg.targetdir}/jit/',
        '"../obj/buildvm/%{cfg.buildcfg}/%{cfg.platform}/buildvm.exe" -m peobj -o "$(IntDir)lj_vm.obj"',
         BuildVmCommand("-m bcdef","lj_bcdef.h", true),
         BuildVmCommand("-m ffdef", "lj_ffdef.h", true),
         BuildVmCommand("-m libdef", "lj_libdef.h", true),
         BuildVmCommand("-m recdef", "lj_recdef.h", true),
         BuildVmCommand("-m folddef", "lj_folddef.h", false).. '"%{sln.location}src/lj_opt_fold.c"',
         BuildVmCommand("-m vmdef", "vmdef.lua", true, '%{cfg.targetdir}/jit/'),
      }
      prebuildmessage"Running pre build commands"
     
      configuration  { "debug"}
         defines { "DEBUG" }
         flags { "Symbols" }
 
      configuration { "release" }
         defines { "NDEBUG"}
         flags { "Symbols" }
         optimize"Speed"

    project "luajit"
        uuid "4E5D480C-3AFF-72E2-23BA-86360FFBF932"
        links { "lua"} 
        kind "ConsoleApp"
        vectorextensions "SSE2"
        defines(flaglist)
        language "C++"
        location "build"
        vpaths { ["libs"] = "src/lib_*.h" }
        vpaths { ["libs"] = "src/lib_*.c" }
      
        files {
            "src/luajit.c"
        }
        
      configuration{"Debug"}        
         defines { "DEBUG" }
         flags { "Symbols" }
 
      configuration{"Release"}
         defines { "NDEBUG"}
         flags { "Symbols" }
         optimize"Speed"
      