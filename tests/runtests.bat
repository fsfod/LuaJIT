@echo off

set TESTDIR=%~dp0
set LJSRC=%~dp0\..\src

setlocal enableextensions enabledelayedexpansion

if not defined INCLUDE (
  echo No MSVC env vars set attempting to find vcvars.bat using system installed vswhere.exe
  set vswhere="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  
  if exist !vswhere! (
    for /f "usebackq tokens=*" %%i in (`!vswhere! -requires Microsoft.VisualStudio.ComponentGroup.NativeDesktop.Core -property installationPath`) do (
      if exist "%%i" set vsdir=%%i
    )

    if exist "!vsdir!\VC\Auxiliary\Build\vcvars64.bat" (
      echo Running vcvars64 found in !vsdir! with vswhere.exe
      call "!vsdir!\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)

if not defined INCLUDE (
  echo No VC compiler found
  EXIT /B 1
)

echo --------- Building LuaJIT --------------------

call :build_luajit "normal"
call :build_luajit "gc64" "gc64"

cd %TESTDIR%

call :test_build "gc64"
call :test_build "normal"

EXIT /B 0

:build_luajit
  pushd .
  cd %LJSRC%
  del *.pdb *.lib *.dll *.exp *.exe
  call msvcbuild.bat %~2
  if exist "luajit.exe" (
    call :copy_luajit "%~1"
  ) ELSE (
    @echo LuaJIT %~1 build failed
    popd
    EXIT /B 1
  )
  popd
EXIT /B 0

:test_build
  pushd .
  cd "%TESTDIR%\LuaJIT-test-cleanup\test"
  "%TESTDIR%\builds\%~1\luajit.exe" test.lua
  if %ERRORLEVEL% NEQ 0 (
    EXIT /B 1
  )
  popd
EXIT /B 0

:copy_luajit
  if not exist "%TESTDIR%\builds\%~1" mkdir "%TESTDIR%\builds\%~1" 
  xcopy luajit.exe "%TESTDIR%\builds\%~1" /y
  xcopy lua51.dll  "%TESTDIR%\builds\%~1" /y
  xcopy lua51.pdb  "%TESTDIR%\builds\%~1" /y
  xcopy lua51.lib  "%TESTDIR%\builds\%~1" /y  
  xcopy jit\*.lua  "%TESTDIR%\builds\%~1\jit" /y /i

  if not exist "%TESTDIR%\builds\%~1\include" mkdir "%TESTDIR%\builds\%~1\include"
  xcopy luaconf.h "%TESTDIR%\builds\%~1\include" /y
  xcopy lua.h     "%TESTDIR%\builds\%~1\include" /y
  xcopy luajit.h  "%TESTDIR%\builds\%~1\include" /y
  xcopy lualib.h  "%TESTDIR%\builds\%~1\include" /y
  xcopy lauxlib.h "%TESTDIR%\builds\%~1\include" /y
EXIT /B 0
