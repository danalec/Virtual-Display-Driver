param(
    # Instance-name pattern identifying the virtual monitor.
    # "MTT1337" was an IddSampleDriver leftover that exists nowhere in this project,
    # so the default follows the driver's own identity instead: the INF DeviceName
    # "Virtual Display Driver" (MttVDD.inf:71). Override it if your EDID reports
    # a different monitor description.
    [Parameter(Mandatory=$false, Position=0)]
    [string]$InstancePattern = '*Virtual*Display*'
)

# One-liner to get the logicalname of the first VDD
# You can alter the script -First 3, would make the third in the list, 
# or -Last 2 would give second from last.

$ErrorActionPreference = 'Stop';
# Version 1.0 on purpose, not Latest: it enforces one rule -- an unassigned variable
# raises instead of silently binding $null -- and that is exactly the rule that
# catches the $rate class of bug. An AST sweep of all 12 helpers found 0 uninitialized
# reads, so enabling it cannot regress anything that works today. Do NOT raise to
# 2.0/Latest without auditing property access first: 2.0 also throws on a missing
# property, and $monitor.LogicalDisplay.DeviceName in get_disp_num.ps1 does.
Set-StrictMode -Version 1.0;

# Get-Monitor ships in the MonitorConfig module. Import it explicitly instead of
# relying on another helper script having loaded it already. The version is pinned
# to the one set-dependencies.ps1 requires.
if (-not (Get-Module -Name MonitorConfig)) {
    try {
        Import-Module MonitorConfig -RequiredVersion '1.0.3' -ErrorAction Stop
    }
    catch {
        throw "The MonitorConfig module is required by this script. Run modules_install.bat (Administrator) or install it with: Install-Module MonitorConfig -RequiredVersion 1.0.3 -Scope CurrentUser. Details: $($_.Exception.Message)"
    }
}

# Sort by the UID digit out of the instance name. The previous key was
# `(($_.InstanceName -match 'UID(\d+)') -and $Matches[1])`, and PowerShell's -and always
# returns [bool] -- it evaluates the right operand and throws the number away. Every instance
# sorted as 0 or 1, in Get-Monitor's emission order, and an instance with no UID digits at all
# (a generic DISPLAY\DEFAULTMONITOR-style name is common) sorted as 0 and won -First 1, so the
# script handed the caller the DeviceName of the wrong display. Disjoint name -> [int]::MaxValue
# so a nameless instance sorts last instead of first.
$monitor = Get-Monitor | Where-Object { $_.InstanceName -like $InstancePattern } | Sort-Object { if ($_.InstanceName -match 'UID(\d+)') { [int]$Matches[1] } else { [int]::MaxValue } } | Select-Object -First 1

if (-not $monitor) {
    Write-Warning "No monitor matched '$InstancePattern'. Run 'Get-Monitor' to list the instance names, then re-run with -InstancePattern <name>."
}
else {
    $dispnum = $monitor.LogicalDisplay.DeviceName

    # Echo the result to console
    Write-Host $dispnum
}

# Pause before closing when run by hand from VDC. This used to fire unconditionally, which
# blocks forever under CI, with stdin redirected, or when the script is called by another
# process -- exactly the cases where someone would script it. Skip it when there is no console
# to pause.
if (-not [Console]::IsInputRedirected -and $host.UI.RawUI) {
    Write-Host "Press any key to continue ..."
    $host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") | Out-Null
}
