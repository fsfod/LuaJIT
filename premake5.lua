local p = premake

local flaglist = {
    --"LUA_USE_ASSERT",
    --"LUA_USE_APICHECK",
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

premake.api.register {
  name = "dynasmflags",
  scope = "config",
  kind = "list:string",
}

premake.api.register {
  name = "bindir",
  scope = "config",
  kind = "path",
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
    for _, path in ipairs(wks.workspace_files) do
      premake.w(path.." = "..path)
    end
    premake.pop("EndProjectSection")
    premake.pop("EndProject")
  end
  base(wks)
end)

p.api.register {
    name = "custombuildcommands",
    scope = "config",
    kind = "list:string",
    tokens = true,
    pathVars = true,
}

p.api.register {
    name = "custombuildinputs",
    scope = "config",
    kind = "list:file",
    tokens = true,
    pathVars = true,
}

p.api.register {
    name = "custombuildoutputs",
    scope = "config",
    kind = "list:file",
    tokens = true,
    pathVars = true,
}

newoption {
    trigger = "builddir",
    description = "override the build directory"
}

newoption {
   trigger     = "host_lua",
   value       = "path",
   description = "Specify the hosts Lua executable to run dynasm during the build instead of building minilua"
}
if os.isfile("user.lua") then
  dofile("user.lua")
end

BuildDir = _OPTIONS["builddir"] or "build"
DEBUG_LUA_PATH = _OPTIONS["DEBUG_LUA_PATH"] or ""
DebugDir = _OPTIONS["debugdir"] or DebugDir or "tests"
DebugArgs = _OPTIONS["debugargs"] or DebugArgs or "../tests/runtests.lua"

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

local buildvminputs = {
    "%{sln.location}src/lib_base.c",
    "%{sln.location}src/lib_math.c",
    "%{sln.location}src/lib_bit.c",
    "%{sln.location}src/lib_string.c",
    "%{sln.location}src/lib_table.c",
    "%{sln.location}src/lib_io.c",
    "%{sln.location}src/lib_os.c",
    "%{sln.location}src/lib_package.c",
    "%{sln.location}src/lib_debug.c",
    "%{sln.location}src/lib_jit.c",
    "%{sln.location}src/lib_ffi.c",
    "%{sln.location}src/lib_ffi.c",
    "%{sln.location}src/vm_x64.dasc",
    "%{sln.location}src/vm_x86.dasc",
}

liblistString = "%{sln.location}/src/"..table.concat(liblist, " %{sln.location}/src/")

--local libs = os.matchfiles("src/lib_*.c")

function BuildVmCommand(cmd, outputfile, addLibList, outputDir)

    outputDir = outputDir or "%{cfg.objdir}/"
    
    local result = '"obj/buildvm/%{cfg.buildcfg}%{cfg.platform}/buildvm%{cfg.system == "windows" and ".exe" or ""}" '..cmd..' -o "'..outputDir..outputfile..'" '

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
  minilua = '"obj/%{cfg.buildcfg}%{cfg.platform}/minilua/minilua%{cfg.system == "windows" and ".exe" or ""}"'
else
  minilua = HOST_LUA
end

if not SlnFileName then
  SlnFileName = "LuaJit"
else
  SlnFileName = "LuaJit_"..SlnFileName
end

workspace "LuaJit"
  filename(SlnFileName)
  editorintegration "On"
  configurations { "Debug", "Release",  "DebugGC64", "ReleaseGC64", "DebugStatic", "ReleaseStatic"}
  platforms { "x64", "x86" }
  objdir "%{sln.location}/%{BuildDir}/obj/%{prj.name}/%{cfg.buildcfg}%{cfg.platform}/"
  targetdir "%{sln.location}/%{BuildDir}/obj/%{prj.name}/%{cfg.buildcfg}%{cfg.platform}/"
  bindir "%{wks.location}/bin/%{cfg.buildcfg}/%{cfg.platform}"
  startproject "luajit"
  workspace_files {
    "lua.natvis",
    ".editorconfig",
  }
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

  filter { "system:windows", "NOT *Static" }
    defines {  "LUA_BUILD_AS_DLL" }

  filter { "tags:NOJIT" }
    defines {  "LUAJIT_DISABLE_JIT" }

  filter "tags:LUA52COMPAT"
    defines { "LUAJIT_ENABLE_LUA52COMPAT" }

  filter "tags:GC64"
    defines { "LUAJIT_ENABLE_GC64" }

  filter "tags:DUALNUM"
    defines {"LUAJIT_NUMMODE=2"}

if not HOST_LUA then
  project "minilua"
    kind "ConsoleApp"
    location(BuildDir)
    configurations { "Release" }
    language "C"
    vpaths { ["Sources"] = "src/host" }
    files {
      "src/host/minilua.c",
    }
    filter { "system:linux" }
      links {
        "m",
      }

    filter "Debug*"
      defines { "NDEBUG" }
      optimize "Speed"

    filter "Release*"
      defines { "NDEBUG" }
      optimize "Speed"
end

  project "buildvm"
    kind "ConsoleApp"
if not HOST_LUA then
    dependson { "minilua" }
end
    vectorextensions "SSE2"
    location(BuildDir)
    language "C"

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
    filter { "*Static" }
      kind "StaticLib"
    filter { "NOT *Static" }
      kind "SharedLib"
    filter {}

    targetdir "%{cfg.bindir}"
    location(BuildDir)
    symbols "On"
    targetname "lua51"
    vectorextensions "SSE2"
    language "c"

    defines(flaglist)
    dependson "buildvm"
    vpaths { ["libs"] = "src/lib_*.h" }
    vpaths { ["libs"] = "src/lib_*.c" }
    vpaths { ["headers"] = "src/lj_*.h" }
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

    filter { "system:windows" }
      custombuildcommands {
        '{MKDIR} %{cfg.targetdir}/jit/',
        '"obj/buildvm/%{cfg.buildcfg}%{cfg.platform}/buildvm.exe" -m peobj -o %{cfg.objdir}lj_vm.obj',
        BuildVmCommand("-m bcdef","lj_bcdef.h", true),
        BuildVmCommand("-m ffdef", "lj_ffdef.h", true),
        BuildVmCommand("-m libdef", "lj_libdef.h", true),
        BuildVmCommand("-m recdef", "lj_recdef.h", true),
        BuildVmCommand("-m folddef", "lj_folddef.h", false).. '%[src/lj_opt_fold.c]',
        BuildVmCommand("-m vmdef", "vmdef.lua", true, '%{cfg.targetdir}/jit/'),
      }
      custombuildoutputs {
        '%{cfg.objdir}lj_bcdef.h',
        '%{cfg.objdir}lj_ffdef.h',
        '%{cfg.objdir}lj_libdef.h',
        '%{cfg.objdir}lj_recdef.h',
        '%{cfg.objdir}lj_folddef.h',
        '%{cfg.objdir}lj_vm.obj',
      }
      custombuildinputs(buildvminputs)

    filter { "system:linux" }
      prebuildcommands {
        '{MKDIR} %{cfg.targetdir}/jit/',
        '"obj/buildvm/%{cfg.buildcfg}%{cfg.platform}/buildvm" -m elfasm -o %{cfg.objdir}/lj_vm.S',
        BuildVmCommand("-m bcdef","lj_bcdef.h", true),
        BuildVmCommand("-m ffdef", "lj_ffdef.h", true),
        BuildVmCommand("-m libdef", "lj_libdef.h", true),
        BuildVmCommand("-m recdef", "lj_recdef.h", true),
        BuildVmCommand("-m folddef", "lj_folddef.h", false).. '%[src/lj_opt_fold.c]',
        BuildVmCommand("-m vmdef", "vmdef.lua", true, '%{cfg.targetdir}/jit/'),
      }

    filter "NOT tags:GC64"
      files { "lua.natvis" }

    filter "tags:GC64"
      files { "lua64.natvis" }

    filter "system:windows"
      linkoptions {'%{cfg.objdir}/lj_vm.obj'}
      
    filter "system:linux"
      links {
        "dl",
      }
      linkoptions {'%{cfg.objdir}/lj_vm.S'}

    filter { "system:windows", "Debug*", "tags:FixedAddr" }
      linkoptions { "/FIXED", "/DEBUG", '/BASE:"0x00440000', "/DYNAMICBASE:NO" }

    filter "Debug*"
      defines { "DEBUG", "LUA_USE_ASSERT" }

    filter  "Release*"
      optimize "Speed"
      defines { "NDEBUG"}


  project "luajit"
    kind "ConsoleApp"
    location(BuildDir)
    targetdir "%{cfg.bindir}"
    vectorextensions "SSE2"
    symbols "On"
    language "C++"

    defines(flaglist)
    vpaths { ["libs"] = "src/lib_*.h" }
    vpaths { ["libs"] = "src/lib_*.c" }
    debugenvs {
      "LUA_PATH=%{cfg.bindir}/?.lua;%{sln.location}src/?.lua;%{sln.location}tests/?.lua"..DEBUG_LUA_PATH..";%LUA_PATH%",
    }

    debugdir(DebugDir)
    debugargs(DebugArgs)

    files {
      "src/luajit.c"
    }

    links {
      "lua"
    }

    filter "Debug*"
      defines { "DEBUG", "LUA_USE_ASSERT" }

    filter "Release*"
      optimize "Speed"
      defines { "NDEBUG"}

    filter { "system:windows", "Debug*", "tags:FixedAddr" }
      linkoptions { "/FIXED", "/DEBUG", '/BASE:"0x00400000',  "/DYNAMICBASE:NO" }

project "CreateRelease"
    kind "Utility"
    location "build"
    targetdir "output"
    dependson "luajit"

    files {
      "src/lauxlib.h",
      "src/lua.h",
      "src/lua.hpp",
      "src/luaconf.h",
      "src/luajit.h",
      "src/lualib.h",
      "src/jit/*.lua",
      "%{cfg.bindir}/jit/vmdef.lua",
    }
    
    filter { "system:windows" }
      files {
        "%{cfg.bindir}/luajit.exe",
        "%{cfg.bindir}/luajit.pdb",
        "%{cfg.bindir}/lua51.dll",
        "%{cfg.bindir}/lua51.pdb",
        "%{cfg.bindir}/lua51.lib",
      }
    filter { "system:linux" }
      files {
        "%{cfg.bindir}/luajit",
        "%{cfg.bindir}/libluajit.so",
        "%{cfg.bindir}/lua51.lib",
      }
    
    filter {'files:**.h*'}
      buildcommands {
        '{COPY} %[%{file.relpath}] %{cfg.targetdir}/include/',
      }
      buildoutputs { 
        "%{cfg.targetdir}/include/%{file.name}",
      }
      buildmessage ""
      
    filter {'files:**.exe or files:**.pdb or files:**.dll or files:**.lib'}
      buildcommands {
        '{COPY} %[%{file.relpath}] %{cfg.targetdir}/',
      }
      buildoutputs { 
        "%{cfg.targetdir}/%{file.name}",
      }
      buildmessage "Copying %{file.path}"
    
    filter {'files:**/jit/*.lua'}
      buildcommands {
        '{COPY} %[%{file.relpath}] %{cfg.targetdir}/jit/',
      }
      buildoutputs { 
        "%{cfg.targetdir}/jit/%{file.name}",
      }
      buildmessage ""
    
    prebuildmessage "Copying files"
    prebuildcommands {
      "{MKDIR} %{cfg.targetdir}/",
      "{MKDIR} %{cfg.targetdir}/include",
      "{MKDIR} %{cfg.targetdir}/jit",
    }

local rootignors = {
  "*.opensdf",
  "*.sdf",
  "*.suo",
  "*.sln",
}

local function mkdir_and_gitignore(dir)
  --Create directorys first so writing the .gitignore doesn't fail
  os.mkdir(dir)
  os.writefile_ifnotequal("*.*", path.join(dir, ".gitignore"))
end

local bin = os.realpath(path.join(os.realpath(BuildDir), "../bin"))
local dotvs = os.realpath(path.join(os.realpath(BuildDir), "../.vs"))
--Write .gitignore to directorys that contain just contain generated files
mkdir_and_gitignore(os.realpath(BuildDir))
mkdir_and_gitignore(bin)
mkdir_and_gitignore(dotvs)

local function pathlist_tostring(cmds, prj, sep)
  local steps = os.translateCommandsAndPaths(cmds, prj.basedir, prj.location)
  return table.implode(steps, "", "", sep)
end

p.override(p.vstudio.vc2010, "buildEvents", function(base, cfg)
  if #cfg.custombuildcommands > 0 then
    p.push('<CustomBuildStep>')
      _x(2,'<Command>%s</Command>', pathlist_tostring(cfg.custombuildcommands, cfg.project, "\r\n"))
      if #cfg.custombuildoutputs > 0 then
        _x(2,'<Outputs>%s</Outputs>', pathlist_tostring(cfg.custombuildoutputs, cfg.project, ";"))
      end
      if #cfg.custombuildinputs > 0 then
        _x(2,'<Inputs>%s</Inputs>', pathlist_tostring(cfg.custombuildinputs, cfg.project, ";"))
      end
    p.pop("</CustomBuildStep>")
  end
  base(cfg)
end)

local function custombuild_settrigger(prj)
  if #prj.custombuildcommands > 0 then
    p.x("<CustomBuildAfterTargets>BuildGenerateSources</CustomBuildAfterTargets>")
  end
end

p.override(p.vstudio.vc2010.elements, "outputProperties", function(base, cfg)
  local calls = base(cfg)
  table.insert(calls, custombuild_settrigger)
  return calls
end)
