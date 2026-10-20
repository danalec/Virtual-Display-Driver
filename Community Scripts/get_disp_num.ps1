param(
    # Instance-name pattern identifying the virtual monitor.
    #
    # The default was '*Virtual*Display*', justified here as "the INF DeviceName
    # Virtual Display Driver (MttVDD.inf:71)". That was the wrong premise: the
    # instance name Get-Monitor reports is derived from the EDID the display
    # answers with, not from the INF. Its shape is
    # DISPLAY\<manufacturer><product>\<instance>&UID<n>, and the manufacturer
    # code comes from bytes 8 and 9 of the EDID. None of the EDIDs in this
    # repository carries "Virtual" or "Display" there:
    #
    #   digihome_edid.bin  0x58 0xB3      lolhero_edid.bin  0x4D 0xD9
    #   8K240HzHDR.edid    0x05 0xD7      the driver's own  0x36 0x94
    #
    # So the pattern could not match the virtual monitor at all. It does not
    # match a real monitor either, since "Virtual" has to come before
    # "Display" and the instance name starts with "DISPLAY\".
    #
    # The default is the driver's own monitor device id, MTT1337. Windows renders a
    # monitor's device id as the EDID manufacturer as three letters followed by the
    # product code as four hex digits, and the product code in the little endian
    # order the EDID stores it in: the three monitors attached to this machine
    # register as AVX0056, GSM59F1 and GSM5C9E, and their EDIDs carry exactly
    # those bytes. The driver forces its own manufacturer and product codes on
    # every EDID it reports -- modifyEdid() in Driver.cpp writes 0x36 0x94 0x37
    # 0x13 into bytes 8 to 11 -- so 0x3694 decodes to the letters MTT and 0x3713
    # renders as 1337. That is the pattern the upstream script used, and it
    # matches the virtual monitor and no physical one.
    #
    # Matching the UID instead matched every monitor on the machine, physical ones
    # included: the instance name of any display carries &UID<n>, and with a
    # single match the script handed back a physical monitor's DeviceName with
    # no warning at all.
    [Parameter(Mandatory=$false, Position=0)]
    [string]$InstancePattern = '*MTT1337*'
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
#
# The check has to be on the version, not on the name. A bare
# "Get-Module -Name MonitorConfig" matches any loaded version, so the import
# below only ran when nothing was loaded at all, and a session that already had
# MonitorConfig 1.0.x or newer ran against that one's output shape instead.
$requiredMonitorConfigVersion = [version]'1.0.3'
$loadedMonitorConfig = Get-Module -Name MonitorConfig | Where-Object { $_.Version -eq $requiredMonitorConfigVersion }
if (-not $loadedMonitorConfig) {
    try {
        Import-Module MonitorConfig -RequiredVersion $requiredMonitorConfigVersion -ErrorAction Stop
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
$matches = @(Get-Monitor | Where-Object { $_.InstanceName -like $InstancePattern } |
    Sort-Object { if ($_.InstanceName -match 'UID(\d+)') { [int]$Matches[1] } else { [int]::MaxValue } })
$monitor = $matches | Select-Object -First 1

if (-not $monitor) {
    Write-Warning "No monitor matched '$InstancePattern'. Run 'Get-Monitor' to list the instance names, then re-run with -InstancePattern <name>."
    # Exiting 0 here told a caller checking $LASTEXITCODE that the script had
    # succeeded, with an empty result and a warning on the warning stream. The
    # two are indistinguishable from the outside, and this script exists to
    # produce a DeviceName.
    exit 1
}
else {
    if ($matches.Count -gt 1) {
        # The pattern found more than one display, so the one returned may not
        # be the virtual one. Say so, and show the names, instead of handing
        # back a DeviceName that may belong to a real monitor.
        Write-Warning "$($matches.Count) monitors matched '$InstancePattern'; returning the first by UID. Use -InstancePattern to narrow it down:"
        foreach ($m in $matches) { Write-Warning "  $($m.InstanceName)" }
    }

    $dispnum = $monitor.LogicalDisplay.DeviceName

    # Echo the result to the success stream, not to the host. Write-Host does
    # not go there, so $(.\get_disp_num.ps1) captured nothing and
    # .\get_disp_num.ps1 > out.txt produced an empty file. A caller that wants
    # the DeviceName got nothing at all.
    Write-Output $dispnum
}

# Pause before closing when run by hand from VDC. This used to fire unconditionally, which
# blocks forever under CI, with stdin redirected, or when the script is called by another
# process -- exactly the cases where someone would script it. Skip it when there is no console
# to pause.
if (-not [Console]::IsInputRedirected -and $host.UI.RawUI) {
    Write-Host "Press any key to continue ..."
    $host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown") | Out-Null
}
