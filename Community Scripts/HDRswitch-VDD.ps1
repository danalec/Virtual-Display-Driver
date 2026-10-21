# Self-elevate the script if required
if (-Not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] 'Administrator')) {
    if ([int](Get-CimInstance -Class Win32_OperatingSystem | Select-Object -ExpandProperty BuildNumber) -ge 6000) {
        $CommandLine = "-File `"" + $MyInvocation.MyCommand.Path + "`" " + $MyInvocation.UnboundArguments
        # Wait for the elevated child and pass its exit code on. Start-Process
        # without -Wait returns immediately, and a bare `exit` sets the parent's
        # exit code to 0 -- so a caller checking $LASTEXITCODE saw success
        # whether the child had worked or not, and also when the elevation
        # itself was refused or the child died.
        $process = Start-Process -FilePath PowerShell.exe -Verb RunAs -ArgumentList $CommandLine -Wait -PassThru
        exit $process.ExitCode
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
#
# The -ErrorAction belongs on Get-DisplayColorInfo. On the pipeline element it
# governs only that element, and the module writes a non-terminating error
# record for an API failure -- which the global $ErrorActionPreference = 'Stop'
# promotes to terminating. The script then died with the raw API error instead
# of reaching the branch that says the display has no BitsPerColorChannel.
$colorInfo = Get-DisplayColorInfo -DisplayId $disp.DisplayId -ErrorAction SilentlyContinue

$bpcc = $colorInfo | Select-Object -ExpandProperty BitsPerColorChannel
$advanced = $colorInfo | Select-Object -ExpandProperty AdvancedColorEnabled

if ($null -eq $advanced) {
    Write-Error "Get-DisplayColorInfo returned no AdvancedColorEnabled for display $($disp.DisplayId) (BitsPerColorChannel: $bpcc)."
}
elseif ($advanced) {
    # The direction comes from the advanced colour state, which is the flag the API
    # reports about HDR. BitsPerColorChannel is the SDR pipeline's depth and says
    # nothing about whether HDR is on: with the recommended force_bit_depth of 10 or 12
    # the SDR path already reported 10 bpc, so the script always chose "disable" and
    # could never turn HDR on; with the shipped value of 8 it always chose "enable" and
    # could never turn it off. Disabling colour that is already disabled succeeds, so the
    # script reported success having changed nothing.
    #
    # Pass the display identifier, not the whole object returned by Get-DisplayInfo.
    Disable-DisplayAdvancedColor -DisplayId $disp.DisplayId
}
else {
    Enable-DisplayAdvancedColor -DisplayId $disp.DisplayId
}
