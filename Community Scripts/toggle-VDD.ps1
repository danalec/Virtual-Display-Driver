 # submitted by zjoasan
 # Self-elevate the script if required
 if (-Not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] 'Administrator')) {
    if ([int](Get-CimInstance -Class Win32_OperatingSystem | Select-Object -ExpandProperty BuildNumber) -ge 6000) {
        $CommandLine = "-File `"" + $MyInvocation.MyCommand.Path + "`" " + $MyInvocation.UnboundArguments
        # Wait for the elevated child and pass its exit code on. Start-Process
        # without -Wait returns immediately, and a bare `exit` sets the parent's
        # exit code to 0 -- so a caller checking $LASTEXITCODE saw success
        # whether the child had worked or not, and also when the elevation
        # itself was refused or the child died.        $process = Start-Process -FilePath PowerShell.exe -Verb RunAs -ArgumentList $CommandLine -Wait -PassThru
        exit $process.ExitCode
    }
 }

$ErrorActionPreference = 'Stop';
# Version 1.0 on purpose, not Latest: it enforces one rule -- an unassigned variable
# raises instead of silently binding $null -- and that is exactly the rule that
# catches the $rate class of bug. An AST sweep of all 12 helpers found 0 uninitialized
# reads, so enabling it cannot regress anything that works today. Do NOT raise to
# 2.0/Latest without auditing property access first: 2.0 also throws on a missing
# property, and $monitor.LogicalDisplay.DeviceName in get_disp_num.ps1 does.
Set-StrictMode -Version 1.0;

# 'Virtual Display Driver' is this repo's INF DeviceName (MttVDD.inf:71), so it matches here.
# The HardwareID fallback covers the other case: silent-install.ps1 installs the upstream release
# package, and any build whose INF names the device differently leaves this matching nothing --
# a silent no-op with only a warning. Root\MttVDD is the ID the INF installs under.
$device = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -eq 'IddSampleDriver Device HDR' -or $_.FriendlyName -eq 'Virtual Display Driver' }
if (-not $device) {
    $device = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
        Where-Object { $_.HardwareID -like 'Root\MttVDD*' }
}

if ($device) {
    if ($device.Status -eq 'OK') {
        Write-Host "Disabling device"; $device | disable-pnpdevice -Confirm:$false
    } else {
        Write-Host "Enabling device"; $device | enable-pnpdevice -Confirm:$false
    }
} else {
    Write-Warning "Device not found"
    # Exiting 0 told a caller checking $LASTEXITCODE that the script had
    # succeeded, with an empty result and a warning on the warning stream.
    # The two are indistinguishable from the outside, and the whole point of
    # this script is to enable or disable the device.
    exit 1
}
