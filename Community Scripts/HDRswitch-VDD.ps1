# Self-elevate the script if required
if (-Not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] 'Administrator')) {
    if ([int](Get-CimInstance -Class Win32_OperatingSystem | Select-Object -ExpandProperty BuildNumber) -ge 6000) {
        $CommandLine = "-File `"" + $MyInvocation.MyCommand.Path + "`" " + $MyInvocation.UnboundArguments
        Start-Process -FilePath PowerShell.exe -Verb Runas -ArgumentList $CommandLine
        Exit
    }
}

# Install dependencies if required
. "$PSScriptRoot\set-dependencies.ps1"

$ErrorActionPreference = 'Stop';
# Version 1.0 on purpose, not Latest: it enforces one rule -- an unassigned variable
# raises instead of silently binding $null -- and that is exactly the rule that
# catches the $rate class of bug. An AST sweep of all 12 helpers found 0 uninitialized
# reads, so enabling it cannot regress anything that works today. Do NOT raise to
# 2.0/Latest without auditing property access first: 2.0 also throws on a missing
# property, and $monitor.LogicalDisplay.DeviceName in get_disp_num.ps1 does.
Set-StrictMode -Version 1.0;

# Getting the correct display. Throws, with the list of displays it did
# find, when the driver is not present.
$disp = Get-VddDisplay

# BitsPerColorChannel is a UInt32, not an Int32. Verified by reflecting over the pinned
# DisplayConfig 1.1.1: Get-DisplayColorInfo returns MartinGC94.DisplayConfig.API.ColorInfo,
# which carries BitsPerColorChannel as UInt32, and Select-Object preserves that type.
#
# The old test was `-is [int]`, and UInt32 is not Int32, so it was false for every real
# value. Both branches were unreachable: the script always fell into the else, where
# write-error under the $ErrorActionPreference = 'Stop' set above made it terminate. The
# toggle could not work at all, which no amount of reading the script would have shown.
#
# Test the value, not the exact CLR type.
$bpcc = Get-DisplayColorInfo -DisplayId $disp.DisplayId |
    Select-Object -ExpandProperty BitsPerColorChannel -ErrorAction SilentlyContinue

if ($null -eq $bpcc) {
    Write-Error "Get-DisplayColorInfo returned no BitsPerColorChannel for display $($disp.DisplayId)."
}
elseif ($bpcc -ge 10) {
    # Pass the display identifier, not the whole object returned by Get-DisplayInfo.
    Disable-DisplayAdvancedColor -DisplayId $disp.DisplayId
}
else {
    Enable-DisplayAdvancedColor -DisplayId $disp.DisplayId
}
