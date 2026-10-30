@echo off
rem Builds RHRT64.exe (x64, D3D12 + DXR 1.1) into bin\RHRayTrace\. Copy that folder next to _gtaRenderHook.asi.
setlocal
set "DXC=%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "delims=" %%i in ('vswhere -latest -products * -property installationPath') do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio / Build Tools not found & exit /b 1)
cd /d "%~dp0"
if not exist bin\RHRayTrace mkdir bin\RHRayTrace
if not exist obj mkdir obj
"%DXC%" -nologo -T cs_6_5 -E TraceAO -O3 -Fh src\TraceAO.h -Vn g_TraceAO src\RTAO.hlsl || exit /b 1
"%DXC%" -nologo -T cs_6_5 -E BlurAO -O3 -Fh src\BlurAO.h -Vn g_BlurAO src\RTAO.hlsl || exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /DNDEBUG /W4 /I..\gtaRenderHook /Foobj\ src\RHRT64.cpp /Fe:bin\RHRayTrace\RHRT64.exe /link /SUBSYSTEM:WINDOWS user32.lib || exit /b 1
echo built bin\RHRayTrace\RHRT64.exe
