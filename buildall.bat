
set msbuildDir=%ProgramFiles(x86)%\MSBuild

set msbuild=%msbuildDir%\14.0\bin\MSBuild.exe


if not defined msbuild echo error: can't find MSBuild.exe & goto :eof
if not exist "%msbuild%" echo error: %msbuild%: not found & goto :eof

IF NOT EXIST LuaJit.sln (
  premake5 vs2013
)

"%msbuild%" buildall.proj /t:BuildAll /m