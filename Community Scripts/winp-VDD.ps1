# Self-elevate the script if required
if (-Not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] 'Administrator')) {
    if ([int](Get-CimInstance -Class Win32_OperatingSystem | Select-Object -ExpandProperty BuildNumber) -ge 6000) {
        $CommandLine = "-File `"" + $MyInvocation.MyCommand.Path + "`" "
        # A switch cannot be built from a string under -File: "-reset:True" fails
        # with "Cannot process argument transformation on parameter 'reset'", and
        # the elevated child dies before the body runs. Pass the name bare when
        # it is on, and leave it out when it is off.
        $MyInvocation.BoundParameters.GetEnumerator() | ForEach-Object {
            if ($_.Value -is [System.Management.Automation.SwitchParameter]) {
                if ($_.Value.IsPresent) { $CommandLine += "-$($_.Key) " }
            }
            else {
                $CommandLine += "-$($_.Key):$($_.Value) "
            }
        }
        # Wait for the elevated child and pass its exit code on. Start-Process
        # without -Wait returns immediately, and a bare `exit` sets the parent's
        # exit code to 0 -- so a caller checking $LASTEXITCODE saw success
        # whether the child had worked or not, and also when the elevation
        # itself was refused or the child died.
        $process = Start-Process -WorkingDirectory $PSScriptRoot -FilePath PowerShell.exe -Verb RunAs -ArgumentList $CommandLine -Wait -PassThru
        exit $process.ExitCode
    }
}

# install dependencies if required
. "$PSScriptRoot\set-dependencies.ps1"

$ErrorActionPreference = 'Stop';
# Version 1.0 on purpose, not Latest: it enforces one rule -- an unassigned variable
# raises instead of silently binding $null -- and that is exactly the rule that
# catches the $rate class of bug. An AST sweep of all 12 helpers found 0 uninitialized
# reads, so enabling it cannot regress anything that works today. Do NOT raise to
# 2.0/Latest without auditing property access first: 2.0 also throws on a missing
# property, and $monitor.LogicalDisplay.DeviceName in get_disp_num.ps1 does.
Set-StrictMode -Version 1.0;

$profi = Get-DisplayProfile

if ( $profi -eq "Extend" ) {
    Set-DisplayProfile Clone
	break
}
if ( $profi -eq "Clone" ) {
    Set-DisplayProfile Extend
	break
}

# Neither profile matched: say so instead of exiting silently.
Write-Error "Current display profile is '$profi', which is neither Extend nor Clone. Nothing was changed.";
