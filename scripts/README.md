# VDD Utilities

This folder is reserved for standalone utility scripts for the Virtual Display Driver.

## Status: no utility scripts here yet

Earlier versions of this file documented a menu-based `Toggle_VDD.bat`, but that script was never
committed to the repository. The helper scripts that did live here were moved to
[`Community Scripts/`](../Community%20Scripts); this folder now holds this README and nothing else.

The functionality it described is available today as PowerShell scripts under
[`Community Scripts/`](../Community%20Scripts):

- [`toggle-VDD.ps1`](../Community%20Scripts/toggle-VDD.ps1) — one-click enable/disable of the driver, plus a status check.
- [`virtual-driver-manager.ps1`](../Community%20Scripts/virtual-driver-manager.ps1) — broader management (install, uninstall, enable, disable, toggle, status).

Both require an **Administrator** PowerShell session. Neither uses the DisplayConfig or
MonitorConfig modules: they work with the built-in PnP cmdlets, and
`virtual-driver-manager.ps1` downloads DevCon from GitHub the first time it needs one, so it
wants network access. The modules that [`modules_install.bat`](../Community%20Scripts/modules_install.bat)
installs are for the other helpers, the ones that go through `Get-VddDisplay`. See the
[`Community Scripts` README](../Community%20Scripts/README.md) for the full list.

```powershell
# from the repository root, in an elevated PowerShell window
& ".\Community Scripts\modules_install.bat"
& ".\Community Scripts\toggle-VDD.ps1"
```

If you add a script to this folder, document it here.
