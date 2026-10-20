@echo off
setlocal

echo Building EDID Parser...
echo.

rem Locate any Visual Studio with the x64 C++ toolset via vswhere rather than probing a
rem hardcoded list of editions and years, which is how the committed EDIDPardseDL.exe came to
rem be unbuildable from this repository: there was no build script at all.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)

if not defined VSPATH (
    echo ERROR: No Visual Studio installation with the x64 C++ toolset was found.
    echo        Install the "Desktop development with C++" workload, then run this again.
    exit /b 1
)

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found under: %VSPATH%
    exit /b 1
)

echo Using Visual Studio at: %VSPATH%
call "%VCVARS%" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed to initialise the build environment.
    exit /b 1
)

pushd "%~dp0"
cl /nologo /EHsc /std:c++17 edid_parser.cpp /Fe:edid_parser.exe
set "BUILD_RC=%ERRORLEVEL%"
popd

if not "%BUILD_RC%"=="0" (
    echo.
    echo Build FAILED.
    exit /b %BUILD_RC%
)

echo.
echo Build successful. Run edid_parser.exe to regenerate monitor_profile.xml.
exit /b 0