local prefix = _OPTIONS["prefix"] or "./dist/pthreads"

function BuildvmCommand(cmd, outputfile)
    
    filter('files:**'..outputfile)
        buildmessage('buildvm '.. "-o ".. outputfile)   
        buildcommands {
          '"../obj/buildvm/%{cfg.buildcfg}/%{cfg.platform}/buildvm.exe" '..cmd..' -o "$(IntDir)'..outputfile..'"'
        }
end

function premake.fileconfig.hasCustomBuildRule(fcfg)
    return fcfg and (#fcfg.buildcommands > 0) --and (#fcfg.buildoutputs > 0)
end

-- A solution contains projects, and defines the available configurations
solution "LuaJit"
   configurations { "Debug", "Release" }
   platforms { "x32", "x64" }
   defines {"_CRT_SECURE_NO_DEPRECATE" }
   objdir "obj/%{prj.name}/%{cfg.buildcfg}/%{cfg.platform}/"
   targetdir "bin/%{cfg.buildcfg}"
   startproject"lua"
   
   project "MiniLua"
      configurations { "Release" }
      location "build"
      kind "ConsoleApp"
      language "C++"
      targetdir "obj/%{prj.name}/%{cfg.buildcfg}/%{cfg.platform}/"
      vpaths { ["Sources"] = "src/host" }
      files {
        "src/host/minilua.c", 
      }

      configuration "Debug"
         defines { "NDEBUG"}
         optimize"Speed"
 
      configuration "Release"
         defines { "NDEBUG"}
         optimize"Speed" 
    

   -- A project defines one build target
   project "buildvm"
      kind "ConsoleApp"
      dependson { "miniLua"} 
      vectorextensions "SSE2"
      location "build"
      language "C++"
      targetdir "obj/%{prj.name}/%{cfg.buildcfg}/%{cfg.platform}/"
      files {
        "src/vm_x86.dasc",
        "src/host/buildvm*.c",
      }
      includedirs{
        "%{cfg.objdir}",
        "src"
      }
      
      filter 'files:src/vm_x86.dasc'
        buildmessage 'Compiling %{file.relpath}'
        buildcommands {
           'start /B /d"../src" "" "../obj/minilua/%{cfg.buildcfg}/%{cfg.platform}/minilua.exe" %{sln.location}dynasm/dynasm.lua -LN -D WIN -D JIT -D FFI -o %{cfg.objdir}buildvm_arch.h %{file.relpath}'
        }
        buildoutputs { '%{cfg.objdir}/buildvm_arch.h' }


      configuration  { "debug", "x32" }
         defines { "DEBUG" }
         optimize"Speed"
 
      configuration { "release", "x32" }
         defines { "NDEBUG"}
         optimize"Speed"
 
   -- A project defines one build target
   project "lua"
      kind "SharedLib"
      buildoptions "/c"
      dependson { "buildvm", "miniLua"} 
      targetname "lua51"
      vectorextensions "SSE2"
      defines { "LUA_BUILD_AS_DLL" }
      language "C++"
      location "build"
      vpaths { ["libs"] = "src/lib_*.h" }
      vpaths { ["libs"] = "src/lib_*.c" }
      
      files {
        "src/lj_*.h",
        "src/lj_*.c",
        "src/lib_*.h",
        "src/lib_*.c",
        
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
        '"../obj/buildvm/%{cfg.buildcfg}/%{cfg.platform}/buildvm.exe" -m peobj -o "$(IntDir)lj_vm.obj"'
      }
      prebuildmessage"Running pre build commands"

      configuration  { "debug", "x32" }
         defines { "DEBUG" }
         flags { "Symbols" }
 
      configuration { "release", "x32" }
         defines { "NDEBUG"}
         flags { "Symbols" }
         optimize"Speed"
         
      
   --[[      
      filter 'files:**lj_vm.obj'
        buildmessage 'buildvm -m peobj lj_vm.obj'      
        buildcommands {
           '"../obj/buildvm/%{cfg.buildcfg}/%{cfg.platform}/buildvm.exe" -m peobj -o "$(IntDir)lj_vm.obj"'
        }
]]

    project "luajit"
        links { "lua"} 
        kind "ConsoleApp"
        vectorextensions "SSE2"
        defines { "LUA_BUILD_AS_DLL" }
        language "C++"
        location "build"
        vpaths { ["libs"] = "src/lib_*.h" }
        vpaths { ["libs"] = "src/lib_*.c" }
      
        files {
            "src/luajit.c"
        }
        
      configuration  { "debug", "x32" }
         
         defines { "DEBUG" }
         flags { "Symbols" }
 
      configuration { "release", "x32" }
         defines { "NDEBUG"}
         flags { "Symbols" }
         optimize"Speed"
        
        --BuildvmCommand("-m bcdef", "lj_bcdef.h")
        
        

--[[        
buildvm -m bcdef -o lj_bcdef.h %ALL_LIB%
BuildvmCommand("-m ffdef"m "lj_ffdef.h %ALL_LIB%
BuildvmCommand("-m libdef -o lj_libdef.h %ALL_LIB%
BuildvmCommand("-m recdef -o lj_recdef.h %ALL_LIB%
BuildvmCommand("-m vmdef -o jit\vmdef.lua %ALL_LIB%
BuildvmCommand("-m folddef -o lj_folddef.h lj_opt_fold.c
]]
         
      