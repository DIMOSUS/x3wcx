@echo off
rem Builds build\x3wcx.wcx (x86), build\x3wcx.wcx64 (x64) and the test drivers build\wcxdrv32.exe, build\wcxdrv64.exe.
rem Needs Visual Studio (or Build Tools) with the C++ workload; set VCVARSALL to point at another vcvarsall.bat.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if defined VCVARSALL goto :found
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARSALL=%%i\VC\Auxiliary\Build\vcvarsall.bat"
:found
if not exist "%VCVARSALL%" (
  echo vcvarsall.bat not found; set VCVARSALL
  exit /b 1
)
if not exist build mkdir build
call :one x86 x3wcx.wcx wcxdrv32.exe || exit /b 1
call :one x64 x3wcx.wcx64 wcxdrv64.exe || exit /b 1
rem install package: open it in Total Commander and it offers to install the plugin
powershell -NoProfile -Command "Compress-Archive -Force -Path build\x3wcx.wcx,build\x3wcx.wcx64,pluginst.inf,README.md,README.ru.md -DestinationPath build\x3wcx.zip" || exit /b 1
echo built: build\x3wcx.wcx build\x3wcx.wcx64 build\x3wcx.zip
exit /b 0

:one
setlocal
call "%VCVARSALL%" %1 >nul 2>nul || exit /b 1
set OBJ=build\obj-%1
if not exist %OBJ% mkdir %OBJ%
set CFLAGS=/nologo /O2 /MT /EHsc /W3 /std:c++17 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
cl %CFLAGS% /LD /Fo%OBJ%\ src\x3wcx.cpp src\flate.cpp /Fe:build\%2 /link /DEF:src\x3wcx.def /IMPLIB:%OBJ%\x3wcx.lib user32.lib || exit /b 1
cl %CFLAGS% /Fo%OBJ%\ tests\wcxdrv.cpp /Fe:build\%3 /link bcrypt.lib || exit /b 1
exit /b 0
