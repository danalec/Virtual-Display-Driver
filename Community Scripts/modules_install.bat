@echo off
setlocal

:: Use PowerShell for elevation check and execution.
:: The path travels in an environment variable, not as a trailing argument:
:: under -Command, PowerShell appends extra arguments to the command text itself,
:: so passing "%~f0" positionally made PowerShell try to *execute* the .bat path
:: (re-running it recursively) while $args stayed empty. $MyInvocation.MyCommand.Path
:: is also empty under -Command, which was the original bug: the re-elevated shell
:: was launched with -File "" and installed nothing while still printing "success".
::
:: The re-elevation launches this .bat itself, not powershell.exe with -File.
:: powershell.exe -File refuses any file that is not a .ps1: it fails with
:: "Processing -File ... failed because the file does not have a '.ps1' extension"
:: and exit code -196608, so the elevated child died before installing anything
:: and the parent reported "Elevated module installation failed".
::
:: It used to launch cmd.exe /c with the path wrapped in a backslash-escaped quote,
:: ('/c', '\"' + $self + '\"'). cmd.exe does not treat a backslash as an escape
:: character, so the argument it received was still \"C:\path\file.bat\" with the
:: backslashes in it, the file was not found, and the elevated child never ran at all.
:: Measured: that form exits 1 and the batch does not run; launching the batch
:: itself goes through the shell, needs no quoting, and returns the batch's own
:: exit code.
set "VDD_INSTALL_BAT=%~f0"

:: The versions are pinned to the same ones set-dependencies.ps1 enforces. Installing "latest"
:: here installed whichever DisplayConfig happened to be current, and the helper scripts break
:: against the newer module -- Get-DisplayColorInfo changed its properties between releases,
:: which is why set-dependencies.ps1 checks the version and not just the name.
set "VDD_DISPLAYCONFIG_VERSION=1.1.1"
set "VDD_MONITORCONFIG_VERSION=1.0.3"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$self = $env:VDD_INSTALL_BAT;" ^
    "$dcv = $env:VDD_DISPLAYCONFIG_VERSION;" ^
    "$mcv = $env:VDD_MONITORCONFIG_VERSION;" ^
    "$elevated = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] 'Administrator');" ^
    "if (-not $elevated) {" ^
        "$p = Start-Process -FilePath $self -Verb RunAs -Wait -PassThru;" ^
        "if ($p.ExitCode -ne 0) { Write-Output ('Elevated module installation failed with exit code ' + $p.ExitCode); exit $p.ExitCode }" ^
        "exit 0" ^
    "} else {" ^
        "try {" ^
            "$ErrorActionPreference = 'Stop';" ^
            "Install-Module -Name DisplayConfig -RequiredVersion $dcv -Scope AllUsers -Force -AllowClobber;" ^
            "Install-Module -Name MonitorConfig -RequiredVersion $mcv -Scope AllUsers -Force -AllowClobber;" ^
            "Write-Output ('Modules installed: DisplayConfig ' + $dcv + ', MonitorConfig ' + $mcv);" ^
        "} catch {" ^
            "Write-Output ('Module installation failed: ' + $_.Exception.Message);" ^
            "exit 1" ^
        "}" ^
    "}"

if %ERRORLEVEL% neq 0 (
    echo Module installation failed.
    exit /b %ERRORLEVEL%
)

:: A bare `pause` blocks forever when this runs from CI, from a scheduled task, or from any
:: caller that redirects stdin -- exactly the cases where anyone would automate it. `choice`
:: with a timeout lets a human read the output without making the script unhangable.
choice /T 10 /D N /M "Press any key to close, or wait 10s..."
:: `choice` returns 255 when the timeout expires with no keypress, and as the
:: last line of this .bat that value became the script's exit code. The parent
:: then saw 255 and reported "Elevated module installation failed" after an
:: install that had succeeded. The result of the install is what the caller
:: needs; the pause must not overwrite it.
exit /b 0