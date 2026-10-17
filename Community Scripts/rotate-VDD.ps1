param(
	[Parameter(Mandatory, Position = 0)]
	[ValidateSet(90, 180, 270)]
	[int]$rotate
)

# Self-elevate the script if required
if (-Not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] 'Administrator')) {
    if ([int](Get-CimInstance -Class Win32_OperatingSystem | Select-Object -ExpandProperty BuildNumber) -ge 6000) {
        $CommandLine = "-File `"" + $MyInvocation.MyCommand.Path + "`" "
        $MyInvocation.BoundParameters.GetEnumerator() | ForEach-Object { $CommandLine += "-$($_.Key):$($_.Value) " }
        Start-Process -WorkingDirectory $PSScriptRoot -FilePath PowerShell.exe -Verb Runas -ArgumentList $CommandLine  
        exit
    }
}

# install dependencies if required.
. "$PSScriptRoot\set-dependencies.ps1"

$ErrorActionPreference = 'Stop';
# Version 1.0 on purpose, not Latest: it enforces one rule -- an unassigned variable
# raises instead of silently binding $null -- and that is exactly the rule that
# catches the $rate class of bug. An AST sweep of all 12 helpers found 0 uninitialized
# reads, so enabling it cannot regress anything that works today. Do NOT raise to
# 2.0/Latest without auditing property access first: 2.0 also throws on a missing
# property, and $monitor.LogicalDisplay.DeviceName in get_disp_num.ps1 does.
Set-StrictMode -Version 1.0;

switch ($rotate) {
	90 {
		$degree = "Rotate90"
		continue
	}
	180 {
		$degree = "Rotate180"
		continue
	}
	270 {
		$degree = "Rotate270"
		continue
	}
	default	{ Write-Error "Invalid rotation angle(valid 90,180,270)."; break }
}


# Getting the correct display
# Throws, with the list of displays it did find, when the driver is not present.
$disp = Get-VddDisplay

# Setting the rotation on the correct display.
Set-DisplayRotation -DisplayId $disp.DisplayId -Rotation $degree
	
