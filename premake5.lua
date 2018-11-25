local p = premake

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
    for _, path in ipairs(wks.workspace_files) do
      premake.w(path.." = "..path)
    end
    premake.pop("EndProjectSection")
    premake.pop("EndProject")
  end
	base(wks)
end)

--p.buildtask = p.api.container("group", p.project)

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

if os.isfile("user.lua") then
  dofile("user.lua")
end

BuildDir = _OPTIONS["builddir"] or "build"

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

liblistString = "%{sln.location}src/"..table.concat(liblist, " %{sln.location}src/")

--local libs = os.matchfiles("src/lib_*.c")

function BuildVmCommand(cmd, outputfile, addLibList, outputDir)
    
    outputDir = outputDir or "%{cfg.objdir}"
    
    local result = '"obj/buildvm/%{cfg.buildcfg}%{cfg.platform}/buildvm.exe" '..cmd..' -o "'..outputDir..outputfile..'" '
    
    if addLibList then
        result = result..liblistString
    end
    
    return result
end

HOST_LUA = _OPTIONS["HOST_LUA"]

if not HOST_LUA then

  if os.isfile(path.join(BuildDir, "minilua.exe")) then
    HOST_LUA = path.join(BuildDir, "minilua.exe")
  elseif os.isfile("minilua.exe") then
    HOST_LUA = "minilua.exe"
  end
  
  if HOST_LUA then
    HOST_LUA = '"'..os.realpath(HOST_LUA)..'"'
  end
end

minilua = HOST_LUA or'"obj/minilua/%{cfg.buildcfg}%{cfg.platform}/minilua.exe"'

DEBUG_LUA_PATH = _OPTIONS["DEBUG_LUA_PATH"] or ""
DebugDir = _OPTIONS["debugdir"] or DebugDir or "tests"
DebugArgs = _OPTIONS["debugargs"] or DebugArgs or "../tests/runtests.lua"

workspace "LuaJit"
  configurations { "Debug", "Release" }
  platforms { "x86", "x64" }
  defines {"_CRT_SECURE_NO_DEPRECATE" }
  objdir "%{sln.location}/%{BuildDir}/obj/%{prj.name}/%{cfg.buildcfg}%{cfg.platform}"
  targetdir "%{sln.location}/%{BuildDir}/obj/%{prj.name}/%{cfg.buildcfg}%{cfg.platform}"
  startproject "luajit"
  dynasmflags {"FFI"}
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
    --tags {"GC64"}  

  filter "system:windows"
    dynasmflags { "WIN" }
    
  filter { "system:windows", "Release" }
      linkoptions { "/Zo" }
 
  filter { "tags:NOJIT" }
    defines {  "LUAJIT_DISABLE_JIT" }
    
  filter {"NOT tags:NOJIT" }
    dynasmflags { "JIT" }

  filter {"NOT tags:GC64", "platforms:x64" }
    dynasmflags { "P64" }
    
  filter "tags:LUA52COMPAT"
    defines { "LUAJIT_ENABLE_LUA52COMPAT" }
    
  filter "tags:GC64"
    defines { "LUAJIT_ENABLE_GC64" }
    
  filter "tags:DUALNUM"
    defines {"LUAJIT_NUMMODE=2"}
    dynasmflags { "DUALNUM" }
 
if not HOST_LUA then  
  project "minilua"
    uuid "74FBF227-E0DA-71C3-E9F2-FC995551D824"
    kind "ConsoleApp"
    location(BuildDir)
    configurations { "Release" }
    language "C"
    vpaths { ["Sources"] = "src/host" }
    files {
      "src/host/minilua.c", 
    }
  
    filter "Debug"
      defines { "NDEBUG" }
      optimize "Speed"
  
    filter "Release"
      defines { "NDEBUG" }
      optimize "Speed" 
end   

  project "buildvm"
    uuid "B86F1F94-244F-9E2F-2D67-290699C50491"
    kind "ConsoleApp"
if not HOST_LUA then
    dependson { "minilua" } 
end
    vectorextensions "SSE2"
    location(BuildDir)
    language "C"
    
    files {
      "src/host/buildvm*.c",
      "src/vm_x64.dasc",
      "src/vm_x86.dasc",
      '%{cfg.objdir}/buildvm_arch.h'
    }
    includedirs{
      "%{cfg.objdir}",
      "src"
    }
    filter { "platforms:x64", "tags:GC64" }
      removefiles {
        "src/vm_x86.dasc" 
      }  
    filter "platforms:x86"
      removefiles  { 
        "src/vm_x64.dasc"
      }

    filter {'files:src/vm_x64.dasc'}
      buildmessage 'Compiling %{file.relpath}'
      buildcommands {
        minilua..' %{sln.location}dynasm/dynasm.lua -LN %{table.implode(cfg.dynasmflags, "-D ", "", " ")} -o %{cfg.objdir}buildvm_arch.h %{file.relpath}'
      }
      buildoutputs { '%{cfg.objdir}/buildvm_arch.h' }
      
    filter {'files:src/vm_x86.dasc'}
      buildmessage 'Compiling %{file.relpath}'
      buildcommands {
        minilua..' %{sln.location}dynasm/dynasm.lua -LN %{table.implode(cfg.dynasmflags, "-D ", "", " ")} -o %{cfg.objdir}buildvm_arch.h %{file.relpath}'
      }
      buildoutputs { '%{cfg.objdir}/buildvm_arch.h' }

    filter {}
      --'%{table.implode(cfg.dynasmflags, "-D ", "", " ")}'
      
      
    filter  {"Debug"}
      optimize "Speed"
 
    filter {"Release"}
      optimize "Speed"
 
  project "lua"
    uuid "C78D880B-3397-887C-BC12-9F7C281B947C"
    kind "SharedLib"
    targetdir "bin/%{cfg.buildcfg}/%{cfg.platform}"
    location(BuildDir)
    buildoptions "/c"
    symbols "On"
    targetname "lua51"
    vectorextensions "SSE2"
    language "C++"
    
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
      --'$(IntDir)lj_vm.obj',--obj/lua/%{cfg.buildcfg}/%{cfg.platform}/
      
      '%{cfg.objdir}/lj_bcdef.h',
      '%{cfg.objdir}/lj_ffdef.h',
      '%{cfg.objdir}/lj_libdef.h',
      '%{cfg.objdir}/lj_recdef.h',
      '%{cfg.objdir}/lj_folddef.h',
    }
    excludes {
      "src/*_arm*",
      "src/*_mips*",
      "src/*_ppc*",
    }

    custombuildcommands {
      '{MKDIR} %{cfg.targetdir}/jit/',
      '"obj/buildvm/%{cfg.buildcfg}%{cfg.platform}/buildvm.exe" -m peobj -o "$(IntDir)lj_vm.obj"',
      BuildVmCommand("-m bcdef","lj_bcdef.h", true),
      BuildVmCommand("-m ffdef", "lj_ffdef.h", true),
      BuildVmCommand("-m libdef", "lj_libdef.h", true),
      BuildVmCommand("-m recdef", "lj_recdef.h", true),
      BuildVmCommand("-m folddef", "lj_folddef.h", false).. '"%{sln.location}src/lj_opt_fold.c"',
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

    filter "NOT tags:GC64"
      files { "lua.natvis" }

    filter "tags:GC64"
      files { "lua64.natvis" } 

    filter "system:windows"
      linkoptions {'"$(IntDir)lj_vm.obj"'}

    filter { "system:windows", "Debug", "tags:FixedAddr" }
      linkoptions { "/FIXED", "/DEBUG", '/BASE:"0x00440000', "/DYNAMICBASE:NO" }
    
    filter "Debug"
      defines { "DEBUG", "LUA_USE_ASSERT" }
  
    filter  "Release"
      optimize "Speed" 
      defines { "NDEBUG"}
      
  
  project "luajit"
    uuid "4E5D480C-3AFF-72E2-23BA-86360FFBF932"
    kind "ConsoleApp"
    location(BuildDir)
    targetdir "bin/%{cfg.buildcfg}/%{cfg.platform}"
    vectorextensions "SSE2"
    symbols "On"
    language "C++"
    
    defines(flaglist)
    vpaths { ["libs"] = "src/lib_*.h" }
    vpaths { ["libs"] = "src/lib_*.c" }
    debugenvs {
      "LUA_PATH=%{sln.location}src/?.lua;%{sln.location}bin/%{cfg.buildcfg}/%{cfg.platform}/?.lua;%{sln.location}tests/?.lua"..DEBUG_LUA_PATH..";%LUA_PATH%",
    }

    debugdir(DebugDir)
    debugargs(DebugArgs)

    files {
      "src/luajit.c"
    }
    
    links { 
      "lua"
    } 
        
    filter "Debug"
      defines { "DEBUG", "LUA_USE_ASSERT" }
 
    filter "Release"
      optimize "Speed"
      defines { "NDEBUG"}

    filter { "system:windows", "Debug", "tags:FixedAddr" }
      linkoptions { "/FIXED", "/DEBUG", '/BASE:"0x00400000',  "/DYNAMICBASE:NO" }


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
