local p = premake

local flaglist = {
    --"LUAJIT_NUMMODE", --1 all number are stored doubles, 2 dual number mode
    "LUAJIT_ENABLE_LUA52COMPAT",
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

premake.api.register {
  name = "dynasmflags",
  scope = "config",
  kind = "list:string",
}

require('vstudio')

premake.api.register {
  name = "workspace_files",
  scope = "workspace",
  kind = "list:string",
}

premake.override(premake.vstudio.sln2005, "projects", function(base, wks)
  if wks.workspace_files and #wks.workspace_files > 0 then
    premake.push('Project("{2150E333-8FDC-42A3-9474-1A3956D46DE8}") = "Solution Items", "Solution Items", "{' .. os.uuid("Solution Items:"..wks.name) .. '}"')
    premake.push("ProjectSection(SolutionItems) = preProject")
    for _, file in ipairs(wks.workspace_files) do
      file = path.rebase(file, ".", wks.location)
      premake.w(file.." = "..file)
    end
    premake.pop("EndProjectSection")
    premake.pop("EndProject")
  end
  base(wks)
end)

newoption {
   trigger     = "host_lua",
   value       = "path",
   description = "Specify the hosts Lua executable to run dynasm during the build instead of building minilua"
}

newoption {
   trigger     = "amalg",
   description = "Build Luajit using a amalg\\unity based build compiling all the sources file as one file",
}

newoption {
   trigger     = "dualnum",
   description = "Build LuaJIT with both a integer and floating point number type"
}

newoption {
   trigger     = "build_luadll",
   description = "Build LuaJIT as a DLL instead of statically linking"
}

TagList = {}

if _OPTIONS.amalg then
  print("Generating amalgamated\\unity based build")
  table.insert(TagList, "AMALG") 
end

if _OPTIONS.dualnum then
  table.insert(TagList, "DUALNUM") 
end

if _OPTIONS.build_luadll then
  table.insert(TagList, "SHARED_LUA")
end

if os.isfile("src/jitlog/build.lua") and os.isfile("src/jitlog/messages.lua") then
  table.insert(TagList, "JITLOG")
end

DebugDir = ""
DebugArgs = ""

BuildDir = _OPTIONS["builddir"] or "build"

local HostExt = ""

if os.host() == "windows" then
  HostExt = ".exe"
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

liblistString = ""

for i, name in ipairs(liblist) do
  liblistString = string.format('%s %%[src/%s]', liblistString, name)
end

--local libs = os.matchfiles("src/lib_*.c")

function BuildVmCommand(cmd, outputfile, addLibList, outputDir)

    outputDir = outputDir or "%{cfg.objdir}/"
    
    local result = '"bin/%{cfg.platform}/%{cfg.buildcfg}/buildvm%{cfg.system == "windows" and ".exe" or ""}" '..cmd..' -o "'..outputDir..outputfile..'" '

    if addLibList then
        result = result..liblistString
    end

    return result
end

HOST_LUA = _OPTIONS["HOST_LUA"]

if HOST_LUA and not os.isfile(HOST_LUA) then
  error("host Lua executable does not exist")
end

if not HOST_LUA then
  local function find_luaexe(exename)
    if os.isfile(path.join(BuildDir, exename)) then
      return path.join(BuildDir, exename)
    elseif os.isfile(exename) then
      return exename
    end
  end
  
  HOST_LUA = find_luaexe("luajit"..HostExt) or find_luaexe("minilua"..HostExt)
end

if HOST_LUA then
  print("Using found Lua executable "..HOST_LUA.." in-place of building minilua")
  HOST_LUA = path.getrelative(BuildDir, HOST_LUA)
end

if not HOST_LUA then
  minilua = '"bin/%{cfg.platform}/%{cfg.buildcfg}/minilua%{cfg.system == "windows" and ".exe" or ""}"'
else
  minilua = HOST_LUA
end

project "*"
  editorintegration "On"
  workspace_files {
    "lua.natvis",
    ".editorconfig",
  }
  tags(TagList)
  defines(flaglist)
  
  filter "platforms:x86"
    architecture "x86"
    defines {
      "LUAJIT_TARGET=LUAJIT_ARCH_X86"
    }

  filter "platforms:x64"
    architecture "x86_64"
    defines {
      "LUAJIT_TARGET=LUAJIT_ARCH_X64"
  }

  filter { "system:windows" }
    defines { 
      "LUAJIT_OS=LUAJIT_OS_WINDOWS", 
      "_CRT_SECURE_NO_DEPRECATE",
    }

  filter { "system:linux" }
    defines { 
      "LUAJIT_OS=LUAJIT_OS_LINUX",
    }
    buildoptions {
      "-fomit-frame-pointer",
      "-fno-stack-protector",
    }

  filter "*GC64*"
    tags { "GC64" }

  filter { "system:windows", "Release*" }
    buildoptions { "/Zo" } -- Ask MSVC for improved debug info for optimized code

  filter { "system:windows", "tags:SHARED_LUA" }
    defines {  "LUA_BUILD_AS_DLL" }

  filter { "tags:NOJIT" }
    defines {  "LUAJIT_DISABLE_JIT" }

  filter "tags:LUA52COMPAT"
    defines { "LUAJIT_ENABLE_LUA52COMPAT" }

  filter "tags:GC64"
    defines { "LUAJIT_ENABLE_GC64" }

  filter "NOT tags:GC64"
    defines { "LUAJIT_DISABLE_GC64" }

  filter "tags:DUALNUM"
    defines {"LUAJIT_NUMMODE=2"}

if not HOST_LUA then
project "minilua"
    kind "ConsoleApp"
    defines { "NDEBUG" }
    optimize "Speed"
    language "C"
    warnings "off"
    vpaths { ["Sources"] = "src/host" }
    files {
      "src/host/minilua.c",
    }
    filter { "system:linux" }
      links {
        "m",
      }
end

project "buildvm"
    kind "ConsoleApp"
if not HOST_LUA then
    dependson { "minilua" }
end
    vectorextensions "SSE2"
    language "C"
    warnings "off"
	defines(flaglist)

    files {
      "src/host/buildvm*.c",
      '%{cfg.objdir}/buildvm_arch.h'
    }
    includedirs{
      "%{cfg.objdir}",
      "src"
    }
    filter { "platforms:x64 or platforms:x86", "NOT tags:GC64" }
      files {
        "src/vm_x86.dasc"
      }
    filter { "platforms:x64", "tags:GC64" }
      files  {
        "src/vm_x64.dasc"
      }

    filter { "system:windows" }
      dynasmflags { "WIN" }

    filter { "NOT tags:GC64", "platforms:x64" }
      dynasmflags { "P64" }

    filter { "tags:DUALNUM" }
      dynasmflags { "DUALNUM" }

    filter { "NOT tags:NOFFI" }
      dynasmflags { "FFI" }

    filter { "NOT tags:NOJIT" }
      dynasmflags { "JIT" }

    filter {'files:src/vm_x64.dasc'}
      buildmessage 'Compiling %{file.relpath}'
      buildcommands {
        minilua..' %[dynasm/dynasm.lua] -LN %{table.implode(cfg.dynasmflags, "-D ", "", " ")} -o %{cfg.objdir}/buildvm_arch.h %{file.relpath}'
      }
      buildoutputs { '%{cfg.objdir}/buildvm_arch.h' }

    filter {'files:src/vm_x86.dasc'}
      buildmessage 'Compiling %{file.relpath}'
      buildcommands {
        minilua..' %[dynasm/dynasm.lua] -LN %{table.implode(cfg.dynasmflags, "-D ", "", " ")} -o %{cfg.objdir}/buildvm_arch.h %{file.relpath}'
      }
      buildoutputs { '%{cfg.objdir}/buildvm_arch.h' }

    filter  {"Debug*"}
      optimize "Speed"

    filter {"Release*"}
      optimize "Speed"

project "lua"
if _OPTIONS.build_luadll then
      kind "SharedLib"
	  targetdir "%{wks.location}/bin/%{cfg.buildcfg}"
else
	kind "StaticLib"
end

	filter { "system:windows" }
	  disablewarnings { "4201", "4127", "4244", "4702", "4706" }
    filter {}

    symbols "On"
    targetname "lua51"
    vectorextensions "SSE2"
    language "c"
    defines(flaglist)
    dependson "buildvm"
	
    vpaths { ["libs"] = "src/lib_*.h" }
    vpaths { ["libs"] = "src/lib_*.c" }
    vpaths { ["headers"] = "src/lj_*.h" }
    vpaths { ["src"] = "src/lj_*.c" }
    vpaths { ["jitlog"] = "src/jitlog/*" }
    vpaths { [""] = "lua.natvis" }
    vpaths { [""] = "lua64.natvis" }

    includedirs {
      "%{cfg.objdir}",
      "src"
    }

    files {
      "src/lj_*.h",
      "src/lj_*.c",
      "src/lib_*.h",
      "src/lib_*.c",

      '%{cfg.objdir}/lj_bcdef.h',
      '%{cfg.objdir}/lj_ffdef.h',
      '%{cfg.objdir}/lj_libdef.h',
      '%{cfg.objdir}/lj_recdef.h',
      '%{cfg.objdir}/lj_folddef.h',
    }

    filter "tags:AMALG"
      files { "src/ljamalg.c" }
    filter {'files:src/lj_*.c or files:src/lib_*.c', 'tags:AMALG'}
      flags {"ExcludeFromBuild"}
    filter {}

    removefiles {
      "src/*_arm.h",
      "src/*_arm64.h",
      "src/*_mips.h",
      "src/*_ppc.h",
    }
    
    filter "system:windows"
      files {
        "bin/%{cfg.platform}/%{cfg.buildcfg}/buildvm.exe"
      }
      
    filter "system:linux"
      files {
        "bin/%{cfg.platform}/%{cfg.buildcfg}/buildvm"
      }
      links {
        "dl",
      }
    
    filter "tags:JITLOG"
      includedirs { 
        "%{cfg.bindir}/jitlog",
      }
      files {
        "src/jitlog/messages.lua",
        "src/jitlog/build.lua",
        "src/jitlog/messages.lua",
        "src/message_readers.lua",
        "src/jitlog/generator.lua",
        "src/jitlog/c_generator.lua",
        "src/jitlog/lua_generator.lua",
        "src/jitlog/cs_generator.lua",
        "%{cfg.bindir}/jitlog/lj_jitlog_def.h",
        "%{cfg.bindir}/jitlog/lj_jitlog_decl.h", 
        "%{cfg.objdir}/lj_jitlog_writers.h",
      }
    
    filter {'files:src/jitlog/messages.lua'}
      buildmessage 'Generating JITLog definitions'
      buildinputs {
         "src/jitlog/build.lua",
         "src/jitlog/messages.lua",
         "src/jitlog/generator.lua",
         "src/jitlog/c_generator.lua",
         "src/jitlog/lua_generator.lua",
         "src/jitlog/cs_generator.lua",
      }
      buildcommands {
        '{MKDIR} %{cfg.targetdir}/jitlog/',
        minilua..' %[src/jitlog/build.lua] %{cfg.tags["GC64"] and "--gc64" or ""} %{file.relpath} writers %{cfg.objdir}/',
        minilua..' %[src/jitlog/build.lua] %{cfg.tags["GC64"] and "--gc64" or ""} %{file.relpath} defs %{cfg.targetdir}/jitlog/',
        minilua..' %[src/jitlog/build.lua] %{cfg.tags["GC64"] and "--gc64" or ""} %{file.relpath} lua %{cfg.targetdir}/jitlog/',
        minilua..' %[src/jitlog/build.lua] %{cfg.tags["GC64"] and "--gc64" or ""} %{file.relpath} csharp %{cfg.targetdir}/jitlog/',
      }
      buildoutputs { 
        "%{cfg.bindir}/jitlog/lj_jitlog_def.h",
        "%{cfg.bindir}/jitlog/lj_jitlog_decl.h", 
        "%{cfg.objdir}/lj_jitlog_writers.h",
      }

    filter { "files:bin/**/buildvm OR files:bin/**/buildvm.exe" }
      buildcommands {
        '{MKDIR} %{wks.location}/bin/%{cfg.buildcfg}/jit/',
        BuildVmCommand("-m bcdef",   "lj_bcdef.h",   true),
        BuildVmCommand("-m ffdef",   "lj_ffdef.h",   true),
        BuildVmCommand("-m libdef",  "lj_libdef.h",  true),
        BuildVmCommand("-m recdef",  "lj_recdef.h",  true),
        BuildVmCommand("-m folddef", "lj_folddef.h", false).. '%[src/lj_opt_fold.c]',
        BuildVmCommand("-m vmdef",   "vmdef.lua",    true, '%{wks.location}/bin/%{cfg.buildcfg}/jit/'),
      }
      buildoutputs {
        '%{cfg.objdir}/lj_bcdef.h',
        '%{cfg.objdir}/lj_ffdef.h',
        '%{cfg.objdir}/lj_libdef.h',
        '%{cfg.objdir}/lj_recdef.h',
        '%{cfg.objdir}/lj_folddef.h',
      }
      buildinputs {
        "src/lib_base.c",
        "src/lib_math.c",
        "src/lib_bit.c",
        "src/lib_string.c",
        "src/lib_table.c",
        "src/lib_io.c",
        "src/lib_os.c",
        "src/lib_package.c",
        "src/lib_debug.c",
        "src/lib_jit.c",
        "src/lib_ffi.c",
        "src/lib_ffi.c",
        "src/vm_x64.dasc",
        "src/vm_x86.dasc",
      }
      
    filter { "files:**/buildvm.exe",  "system:windows" }
      buildcommands {
        '"bin/%{cfg.platform}/%{cfg.buildcfg}/buildvm.exe" -m peobj -o %{cfg.objdir}lj_vm.obj'
      }
      buildoutputs {
        "%{cfg.objdir}/lj_vm.obj",
      }
    
    filter { "files:**/buildvm",  "system:linux" }
      buildcommands {
        '"bin/%{cfg.platform}/%{cfg.buildcfg}/buildvm" -m elfasm -o %{cfg.objdir}/lj_vm.S'
      }
      buildoutputs {
        "%{cfg.objdir}/lj_vm.S",
      }

    filter "NOT tags:GC64"
      files { "lua.natvis" }

    filter "tags:GC64"
      files { "lua64.natvis" }

    filter "Debug*"
      defines { "DEBUG", "LUA_USE_ASSERT" }

    filter  "Release*"
      optimize "Speed"
      defines { "NDEBUG"}