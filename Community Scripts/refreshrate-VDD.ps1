param(
    [Parameter(Mandatory, Position = 0)]
    # Exactly the refresh rates the driver can report, and nothing else. Both sources agree once
    # you check them: <g_refresh_rate> in vdd_settings.xml ships 60/90/120/144/165/240, and
    # option.txt -- the fallback used when no XML is present -- carries 60/120/144/240.
    # 480 and 500 were offered here once and named no mode at all; 30 and 75 survived that cleanup
    # for the same reason. 30 only exists in the XML inside the upstream 25.7.23 release package,
    # which reports 244 rather than 240, and 75 is in neither. Asking for one gets an IddCx error,
    # not a different display.
    [ValidateSet(
        60,
        90,
        120,
        144,
        165,
        240)]
    [int]$refreshrate
)

$ErrorActionPreference = 'Stop';
# Version 1.0 on purpose, not Latest: it enforces one rule -- an unassigned variable
# raises instead of silently binding $null -- and that is exactly the rule that
# catches the $rate class of bug. An AST sweep of all 12 helpers found 0 uninitialized
# reads, so enabling it cannot regress anything that works today. Do NOT raise to
# 2.0/Latest without auditing property access first: 2.0 also throws on a missing
# property, and $monitor.LogicalDisplay.DeviceName in get_disp_num.ps1 does.
Set-StrictMode -Version 1.0;

# Self-elevate the script if required
if (-Not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] 'Administrator')) {
    if ([int](Get-CimInstance -Class Win32_OperatingSystem | Select-Object -ExpandProperty BuildNumber) -ge 6000) {
        $CommandLine = "-File `"" + $MyInvocation.MyCommand.Path + "`" "
        $MyInvocation.BoundParameters.GetEnumerator() | ForEach-Object { $CommandLine += "-$($_.Key):$($_.Value) " }
        Start-Process -WorkingDirectory $PSScriptRoot -FilePath PowerShell.exe -Verb Runas -ArgumentList $CommandLine  
        exit
    }
}

# install dependencies if required
. "$PSScriptRoot\set-dependencies.ps1"

# Getting the correct display
# Throws, with the list of displays it did find, when the driver is not present.
$disp = Get-VddDisplay

# Setting the refresh rate on the correct display.
# The parameter is declared as [int]$refreshrate; $rate never existed and bound as $null.
Set-DisplayRefreshRate -DisplayId $disp.DisplayId -RefreshRate $refreshrate
