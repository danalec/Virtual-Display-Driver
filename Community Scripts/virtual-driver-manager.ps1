<#
.SYNOPSIS
A comprehensive script to manage the Virtual Display Driver.
It can install, uninstall, enable, disable, toggle, and check the status of the driver.

.DESCRIPTION
This script handles the full lifecycle of the Virtual Display Driver.
- If no action is specified, it interactively prompts the user to select one.
- For install/uninstall, it automatically resolves the correct version of Microsoft's DevCon utility using a nearest-build matching algorithm for maximum compatibility.
- For enable/disable/toggle/status, it uses fast, built-in PowerShell commands.
- It requires Administrator privileges and will self-elevate if needed by re-launching in a new window.
- All temporary files are automatically cleaned up unless in Verbose mode for diagnostics.

.PARAMETER Action
Specifies the operation to perform. If omitted, the script will prompt for a selection.

.PARAMETER DriverVersion
Used only with the 'install' action to specify a version of the Virtual Display Driver, otherwise defaults to 'latest'.

.PARAMETER Json
If present, all output will be in JSON format for easy parsing by other programs.

.PARAMETER Silent
If present, the script will close automatically without prompting for a key press. By default, the script will pause for review before closing.

.PARAMETER Verbose
If present, prints detailed diagnostic information and prevents the temporary folder from being deleted for inspection. Provided by [CmdletBinding()].

.EXAMPLE
# --- USAGE WITHIN A POWERSHELL CONSOLE ---

# Run without an action to get an interactive menu. The window will pause when finished.
.\virtual-driver-manager.ps1

# Install the driver and pause for review afterwards (default behavior).
.\virtual-driver-manager.ps1 -Action install

# Uninstall the driver and close the window automatically.
.\virtual-driver-manager.ps1 -Action uninstall -Silent

.EXAMPLE
# --- USAGE FROM CMD.EXE OR ANOTHER PROCESS ---

# Check driver status and get JSON output for automation (no pause).
powershell.exe -ExecutionPolicy Bypass -File .\virtual-driver-manager.ps1 -Action status -Json

# Enable the driver and have the window close automatically.
powershell.exe -ExecutionPolicy Bypass -File .\virtual-driver-manager.ps1 -Action enable -Silent
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $false)]
    [ValidateSet('install', 'uninstall', 'enable', 'disable', 'toggle', 'status')]
    [string]$Action,

    [Parameter(Mandatory = $false)]
    [string]$DriverVersion = "latest",

    [Parameter(Mandatory = $false)]
    [switch]$Json,

    [Parameter(Mandatory = $false)]
    [switch]$Silent
)

$ErrorActionPreference = 'Stop';
# Version 1.0 on purpose, not Latest: it enforces one rule -- an unassigned variable
# raises instead of silently binding $null -- and that is exactly the rule that
# catches the $rate class of bug. An AST sweep of all 12 helpers found 0 uninitialized
# reads, so enabling it cannot regress anything that works today. Do NOT raise to
# 2.0/Latest without auditing property access first: 2.0 also throws on a missing
# property, and $monitor.LogicalDisplay.DeviceName in get_disp_num.ps1 does.
Set-StrictMode -Version 1.0;

#----------------------------------------------------------------------
# SECTION 1: CENTRALIZED LOGGING FUNCTION
#----------------------------------------------------------------------
# This helper function centralizes all script output.
# It supports both colorful console output for humans and JSON for automation.
function Write-Log {
    param(
        [string]$Message,
        [string]$Status = 'Info',
        [string]$Color = 'Cyan'
    )

    if ($Json.IsPresent) {
        $logObject = [PSCustomObject]@{
            status  = $Status.ToLower()
            message = $Message
        }
        $logObject | ConvertTo-Json -Compress | Write-Output
    }
    else {
        switch ($Status) {
            'Success' { Write-Host $Message -ForegroundColor Green }
            'Warning' { Write-Warning $Message }
            # -ErrorAction Continue keeps the logger itself from becoming a terminating
            # error once $ErrorActionPreference is 'Stop': the caller decides how to
            # react to a failed status, this helper only reports it.
            'Error'   { Write-Error $Message -ErrorAction Continue }
            default   { Write-Host $Message -ForegroundColor $Color }
        }
    }
}

#----------------------------------------------------------------------
# SECTION 2: INTERACTIVE ACTION PROMPT (IF NEEDED)
#----------------------------------------------------------------------
# If no action is specified, prompt the user to select one.
# This must happen before the self-elevation check so the chosen action is passed to the elevated process.
if (-not $PSBoundParameters.ContainsKey('Action')) {
    # Prompts are not suitable for automation or non-interactive shells.
    if ($Json.IsPresent -or ($host.UI.RawUI -eq $null)) {
        Write-Log -Message "The -Action parameter is mandatory for non-interactive or JSON mode execution." -Status 'Error'
        exit 1
    }

    $options = 'install', 'uninstall', 'enable', 'disable', 'toggle', 'status'
    Write-Host "`nPlease select an action to perform:" -ForegroundColor Yellow

    for ($i = 0; $i -lt $options.Length; $i++) {
        Write-Host ("  [{0}] {1}" -f ($i + 1), (Get-Culture).TextInfo.ToTitleCase($options[$i]))
    }

    [int]$choice = 0
    while ($choice -lt 1 -or $choice -gt $options.Length) {
        $input = Read-Host -Prompt "`nEnter the number for your choice"
        if (-not ([int]::TryParse($input, [ref]$choice))) {
            $choice = 0 # Invalidate if input is not a number
        }
    }
    # Set the action from the user's choice and add it to PSBoundParameters.
    # This is critical to ensure the self-elevation logic works correctly.
    $Action = $options[$choice - 1]
    $PSBoundParameters.Add('Action', $Action) | Out-Null
    Write-Verbose "User selected action: '$Action'"
    Write-Host "" # Add a newline for better formatting
}


#----------------------------------------------------------------------
# SECTION 3: SELF-ELEVATE TO ADMINISTRATOR
#----------------------------------------------------------------------
# Check if the current user has Administrator rights.
if (-Not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]'Administrator')) {
    Write-Log -Message "Administrator rights are required. Requesting elevation..." -Status 'Warning'

    # The script must re-launch itself as an Administrator to function.
    # We must rebuild the argument list to pass all original parameters to the new elevated process.
    $psArgs = @("-ExecutionPolicy", "Bypass", "-NoProfile", "-File", "`"$($MyInvocation.MyCommand.Path)`"")

    foreach ($key in $PSBoundParameters.Keys) {
        $value = $PSBoundParameters[$key]
        # A switch bound as $false is still a SwitchParameter, so testing the type
        # lost the value: -Json:$false reached the child as a bare -Json, which
        # turned JSON mode on, -Verbose:$false skipped the temp-directory
        # cleanup, and -Confirm:$false made the elevated child stop and ask.
        # Switches are only passed bare when they are actually on.
        if ($value -is [System.Management.Automation.SwitchParameter]) {
            # The name, and only when the switch is on. The elevated child
            # is Windows PowerShell 5.1, and it cannot bind an explicit switch
            # value under -File: "-Json:False" fails with "Cannot process
            # argument transformation on parameter 'Json'" and the child dies
            # before the body runs. An off switch is left out, which is what
            # the child's own default already says.
            if ($value.IsPresent) {
                $psArgs += "-$key"
            }
        }
        else {
            $psArgs += "-$key"
            $psArgs += $value
        }
    }

    Write-Verbose "Re-launching with arguments: $($psArgs -join ' ')"

    # Start the new process with the 'Runas' verb to trigger the UAC elevation prompt.
    $elevatedProcess = Start-Process "powershell.exe" -ArgumentList $psArgs -Verb Runas -Wait -PassThru

    # Pass the exit code from the elevated process back to the original caller.
    $ActionTitleCase = (Get-Culture).TextInfo.ToTitleCase($Action)
    if ($elevatedProcess.ExitCode -eq 0) { Write-Log -Message "$ActionTitleCase Succeeded." -Status 'Success' }
    else { Write-Log -Message "$ActionTitleCase Failed. See the elevated window for details." -Status 'Error' }
    exit $elevatedProcess.ExitCode
}

#----------------------------------------------------------------------
# SECTION 4: SETUP AND GUARANTEED CLEANUP
#----------------------------------------------------------------------
# Create a unique temporary directory to avoid conflicts if the script is run multiple times concurrently.
$tempDir = Join-Path $env:TEMP "VDD-Manager-$(Get-Random)"
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
Write-Verbose "Created temporary directory at $tempDir"

# Use a try/catch/finally block to ensure that no matter what happens (success or error),
# the 'finally' block will ALWAYS run to clean up temporary files.
try {
    Write-Verbose "Script running with Administrator privileges."
    Write-Verbose "Parameters: -Action $Action -DriverVersion $DriverVersion -Json:$($Json.IsPresent) -Silent:$($Silent.IsPresent)"

    # Technical Choice: Force PowerShell to use modern TLS 1.2 for all web requests.
    # This is critical for communicating with modern secure sites like GitHub.
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Write-Verbose "Set security protocol to TLS 1.2 for all web requests."

    #----------------------------------------------------------------------
    # SECTION 5: HELPER FUNCTION TO FIND THE DEVICE
    #----------------------------------------------------------------------
    function Get-VirtualDisplayDevice {
        # Find the device by its user-friendly name first for readability.
        # The query is not silenced: a PnP service that is faulted or a WMI
        # error made this return $null, and the caller reported "Driver is not
        # installed" for a driver that was installed and working. A failure to
        # ask is not an answer of "no".
        $device = Get-PnpDevice -Class Display -ErrorAction Stop | Where-Object {
            $_.FriendlyName -in ('IddSampleDriver Device HDR', 'Virtual Display Driver')
        }
        # Fallback: If not found by name, use the more specific and stable Hardware ID.
        if (-not $device) {
            $device = Get-PnpDevice -HardwareID "Root\MttVDD" -ErrorAction Stop
        }
        return $device
    }

    #---------------------------------------------------------------------- 
    # SECTION 5b: HELPER FUNCTION TO PICK THE DRIVER ASSET
    #---------------------------------------------------------------------- 
    # SHA-256 of the driver archives, per release. silent-install.ps1 pins the
    # same two files for release 25.7.23.
    $knownDriverHashes = @{
        '25.7.23' = @{
            'VirtualDisplayDriver-x86.Driver.Only.zip'   = 'e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a'
            'VirtualDisplayDriver-ARM64.Driver.Only.zip' = 'dcd487161469c726bab60f174a7e7f70ee02afd437cad0d5c0e6755953de5a25'
        }
    }
    # The release publishes the driver as "VirtualDisplayDriver-<arch>.Driver.Only.zip".
    # The x86-named asset is the one x64/AMD64 machines use (the name is upstream's);
    # ARM64 has its own. Matching only "x64.zip" - as this script used to - matched no
    # asset in any current release, so -Action install always failed.
    function Select-DriverAsset {
        param(
            [Parameter(Mandatory = $true)]
            [object]$Release,
            [Parameter(Mandatory = $false)]
            [switch]$Arm64
        )

        $driverAssets = @($Release.assets | Where-Object { $_.name -like 'VirtualDisplayDriver-*.zip' })
        if ($driverAssets.Count -eq 0) { return $null }

        if ($Arm64) {
            $armAsset = $driverAssets | Where-Object { $_.name -like '*-ARM64.*' } | Select-Object -First 1
            if ($armAsset) { return $armAsset }
        }

        $defaultAsset = $driverAssets | Where-Object { $_.name -like '*-x86.*' } | Select-Object -First 1
        if ($defaultAsset) { return $defaultAsset }

        # No asset for this architecture. Falling back to the first one would
        # hand an x64 host the ARM64 package -- the ARM64 asset sorts first by
        # name -- and a host that cannot load the driver it just installed is
        # worse than a clear error. The caller reports which assets the
        # release does have.
        return $null
    }

    #----------------------------------------------------------------------
    # SECTION 6: MAIN ACTION LOGIC
    #----------------------------------------------------------------------
    if ($Action -in @('install', 'uninstall')) {

        #--- 6a. Automatically Resolve DevCon Hash ---
        Write-Log -Message "Action '$Action' requires the DevCon utility. Determining correct version..."
        $osInfo = Get-CimInstance -ClassName Win32_OperatingSystem
        $osBuild = [int]$osInfo.BuildNumber
        
        $osMajorVersion = 'Server'
        if ($osInfo.Caption -match 'Windows 11') { $osMajorVersion = '11' }
        elseif ($osInfo.Caption -match 'Windows 10') { $osMajorVersion = '10' }
        
        Write-Log -Message "Detected: Windows $osMajorVersion (Build $osBuild)"
        Write-Verbose "OS detection complete. Starting DevCon source matching."

        # This map is the core of the matching logic. It translates the human-readable version names
        # from the devcon_sources.json file into their corresponding OS build numbers for comparison.
        $versionNameToBuildMap = @{
            "Windows 11 version 24H2" = 26100; "Windows 11 version 23H2" = 22631; "Windows 11 version 22H2" = 22621;
            "Windows 11 version 21H2" = 22000; "Windows Server 2022" = 20348; "Windows 10 version 2004" = 19041;
            "Windows 10 version 1903" = 18362; "Windows 10 version 1809" = 17763; "Windows Server 2019" = 17763
        }
        
        $sourcesUrl = "https://raw.githubusercontent.com/Drawbackz/DevCon-Installer/refs/heads/master/devcon_sources.json"
        Write-Verbose "Fetching DevCon sources from $sourcesUrl"
        $devconSources = Invoke-RestMethod -Uri $sourcesUrl -ErrorAction Stop

        # Enrich the downloaded source list with a calculated 'BuildNumber' property to make it sortable.
        $enrichedSources = $devconSources | ForEach-Object {
            $source = $_; $matchedBuild = 0
            foreach ($entry in $versionNameToBuildMap.GetEnumerator()) {
                if ($source.Name.Contains($entry.Key)) { $matchedBuild = $entry.Value; break }
            }
            $source | Add-Member -MemberType NoteProperty -Name "BuildNumber" -Value $matchedBuild; $source
        } | Where-Object { $_.BuildNumber -gt 0 }

        $osFamilySources = $enrichedSources | Where-Object { $_.Name -like "*Windows $osMajorVersion*" -or ($osMajorVersion -eq 'Server' -and $_.Name -like "*Server*") }
        
        # --- NEAREST-BUILD MATCHING LOGIC ---
        # 1. Try for a perfect match first.
        $bestMatch = $osFamilySources | Where-Object { $_.BuildNumber -eq $osBuild } | Select-Object -First 1

        if (-not $bestMatch) {
            Write-Log -Message "No exact DevCon match for build $osBuild. Finding nearest available version..." -Status 'Warning'
            # 2. If no exact match, find the newest version that is still older than (or equal to) the current OS.
            #    This is the safest fallback, as it guarantees API compatibility.
            $bestOlderMatch = $osFamilySources | Where-Object { $_.BuildNumber -le $osBuild } | Sort-Object BuildNumber -Descending | Select-Object -First 1
            
            # 3. If no older versions exist, find the oldest version that is newer than the current OS.
            #    This is a less-safe fallback but better than failing completely.
            $bestNewerMatch = $osFamilySources | Where-Object { $_.BuildNumber -gt $osBuild } | Sort-Object BuildNumber | Select-Object -First 1
            
            if ($bestOlderMatch) { $bestMatch = $bestOlderMatch } elseif ($bestNewerMatch) { $bestMatch = $bestNewerMatch }
            Write-Verbose "Nearest older match: $($bestOlderMatch.Name) | Nearest newer match: $($bestNewerMatch.Name)"
        }

        if (-not $bestMatch) { throw "Could not find any compatible DevCon versions for your OS (Build $osBuild)." }
        
        # Prefer a DevCon build matching the OS architecture and fall back to X64 rather than
        # failing: x64 runs through emulation on ARM64. This used to hardcode X64, so the ARM64
        # path downloaded an emulated installer even when the sources list carries a native one.
        $osArchName = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        $archCandidates = switch ($osArchName) {
            'Arm64' { @('ARM64', 'Arm64', 'X64') }
            'X86'   { @('X86', 'x86', 'X64') }
            default { @('X64', 'x64') }
        }
        $devconHash = $null; $devconArch = $null
        foreach ($candidate in $archCandidates) {
            $hash = ($bestMatch.Sources | Where-Object { $_.Architecture -eq $candidate }).Sha256
            if ($hash) { $devconHash = $hash; $devconArch = $candidate; break }
        }
        Write-Log -Message "Using DevCon Source: $($bestMatch.Name) (Build $($bestMatch.BuildNumber))" -Status 'Success'
        Write-Verbose "Selected DevCon Hash ($devconArch): $devconHash"
        if (-not $devconHash) {
            throw "Could not find a DevCon hash for $($archCandidates -join '/') in the selected source: $($bestMatch.Name). Sources offer: $(($bestMatch.Sources.Architecture) -join ', ')"
        }

        #--- 6b. Acquire DevCon Utility ---
        Write-Log -Message "Acquiring secure DevCon utility..."
        $devconInstallerUrl = "https://github.com/Drawbackz/DevCon-Installer/releases/download/1.4-rc/Devcon.Installer.exe"
        # Devcon.Installer.exe is not Authenticode-signed (Get-AuthenticodeSignature reports
        # NotSigned), so a pinned SHA-256 is the only integrity control available for it. This
        # pins the artifact that was reviewed; it does not by itself vouch for the publisher.
        $devconInstallerSha256 = "02266cbe19253db3b6e4095dca49ff315456415d54ebc8e76650194d91e197c7"
        $devconInstallerPath = Join-Path $tempDir "Devcon.Installer.exe"
        Write-Verbose "Downloading DevCon Installer from $devconInstallerUrl"
        Invoke-WebRequest -Uri $devconInstallerUrl -OutFile $devconInstallerPath -UseBasicParsing -ErrorAction Stop
        $devconInstallerActual = (Get-FileHash -Path $devconInstallerPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($devconInstallerActual -ne $devconInstallerSha256) {
            Remove-Item -Path $devconInstallerPath -Force -ErrorAction SilentlyContinue
            throw "Devcon.Installer.exe SHA-256 mismatch: expected $devconInstallerSha256, got $devconInstallerActual. Aborting before executing it."
        }
        Write-Verbose "DevCon Installer downloaded and SHA-256 verified."
        
        # Use the hash to ensure the DevCon-Installer utility downloads the correct, secure version of DevCon.
        $devconArgs = "install -hash $devconHash -update -dir `"$tempDir`""
        Write-Verbose "Running DevCon Installer with arguments: $devconArgs"
        Start-Process -FilePath $devconInstallerPath -ArgumentList $devconArgs -Wait -NoNewWindow
        
        $devconExe = Join-Path $tempDir "devcon.exe"
        if (-not (Test-Path $devconExe)) { throw "Failed to acquire devcon.exe." }
        Write-Verbose "devcon.exe acquired successfully at $devconExe"

        #--- 6c. Execute Install or Uninstall ---
        if ($Action -eq 'install') {
            Write-Log -Message "Starting driver installation..."

            # Resolve the release through the GitHub API instead of guessing an asset
            # file name. The old "Signed-Driver-v<version>-x64.zip" URL returns HTTP 404
            # for every current tag.
            $apiUrl = "https://api.github.com/repos/VirtualDrivers/Virtual-Display-Driver/releases/latest"
            if ($DriverVersion -ne "latest") {
                $apiUrl = "https://api.github.com/repos/VirtualDrivers/Virtual-Display-Driver/releases/tags/$DriverVersion"
                Write-Verbose "Using specified driver version: $DriverVersion"
            }

            # Technical Choice: Many APIs, including GitHub's, require a User-Agent header.
            # Omitting this can lead to connection errors (like 403 Forbidden or 404 Not Found).
            $headers = @{ "User-Agent" = "PowerShell-VDD-Manager-Script" }

            Write-Verbose "Querying GitHub API for driver release: $apiUrl"
            $releaseInfo = Invoke-RestMethod -Uri $apiUrl -Headers $headers -ErrorAction Stop
            Write-Verbose "API call successful. Release found: $($releaseInfo.tag_name)"

            # RuntimeInformation.OSArchitecture, not Win32_Processor.Architecture: on ARM64
            # Windows running an x64-emulated PowerShell the latter can report x64 while the OS
            # is still ARM64, which then selects the AMD64 asset. The upstream package's x86-named
            # zip contains an AMD64 MttVDD.dll (PE machine type 0x8664), so no native ARM64 driver
            # would be installed at all. silent-install.ps1 already documents this exact hazard.
            $cpuArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
            $arm64 = ($cpuArchitecture -eq 'Arm64')
            Write-Verbose "OS architecture: $cpuArchitecture (ARM64: $arm64)"

            $asset = Select-DriverAsset -Release $releaseInfo -Arm64:$arm64
            if (-not $asset) {
                $available = ($releaseInfo.assets | ForEach-Object { $_.name }) -join ', '
                throw "Could not find a Virtual Display Driver asset in release '$($releaseInfo.tag_name)'. Available assets: $available"
            }

            $downloadUrl = $asset.browser_download_url
            Write-Verbose "Found driver asset: $($asset.name)"

            Write-Verbose "Downloading driver from URL: $downloadUrl"
            $driverZipPath = Join-Path $tempDir "driver.zip"
            Invoke-WebRequest -Uri $downloadUrl -OutFile $driverZipPath -UseBasicParsing -ErrorAction Stop
            if (-not (Test-Path $driverZipPath)) { throw "Driver archive was not downloaded from: $downloadUrl" }
            Write-Verbose "Driver ZIP file downloaded to $driverZipPath"

            # Verify what was fetched. The DevCon download below is checked
            # against a hash, and silent-install.ps1 checks all three of its
            # artifacts; this one did not, which made it the only download in
            # this file that expanded an archive of unknown provenance. A
            # release with no hash listed here stops the install and says so.
            $expectedHash = $null
            if ($knownDriverHashes.ContainsKey($releaseInfo.tag_name)) {
                $expectedHash = $knownDriverHashes[$releaseInfo.tag_name][$asset.name]
            }
            if (-not $expectedHash) {
                throw "No known SHA-256 for '$($asset.name)' in release '$($releaseInfo.tag_name)'. This release is newer than the one the hashes were taken from, so the archive cannot be verified. Pin a known release with -DriverVersion, or add the hash to `$knownDriverHashes in this script."
            }
            $actualHash = (Get-FileHash -Path $driverZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actualHash -ne $expectedHash.ToLowerInvariant()) {
                throw "The driver archive downloaded from $downloadUrl has SHA-256 $actualHash, expected $expectedHash. The download was not what it claimed to be, and it was not expanded."
            }
            Write-Verbose "Driver archive SHA-256 verified: $actualHash"

            Expand-Archive -Path $driverZipPath -DestinationPath $tempDir -Force -ErrorAction Stop
            Write-Verbose "Driver archive expanded."

            # Resolve the INF rather than assuming it sits at the root of the archive.
            $driverInf = Join-Path $tempDir "MttVDD.inf"
            if (-not (Test-Path $driverInf)) {
                $foundInf = Get-ChildItem -Path $tempDir -Filter 'MttVDD.inf' -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
                if (-not $foundInf) { throw "MttVDD.inf not found in the extracted driver package under $tempDir." }
                $driverInf = $foundInf.FullName
            }

            # Use DevCon to install the driver by pointing to its INF file and specifying its unique Hardware ID.
            # "Root\MttVDD" identifies this as a root-enumerated virtual device.
            Write-Verbose "Running DevCon to install the driver..."
            if ($PSCmdlet.ShouldProcess("Root\MttVDD", "Install Virtual Display Driver")) {
                & $devconExe install $driverInf "Root\MttVDD"
                # A non-zero exit from a native command is not an error for PowerShell 5.1: there
                # $PSNativeCommandUseErrorActionPreference does not exist there, and the try/catch
                # below never sees it. Silent-install.ps1 already checks this explicitly; without
                # it a refused install (Root\MttVDD already in use, INF rejected, driver store
                # busy) fell through to "completed successfully" with exit code 0, and the runtime
                # config got copied as if a driver were present.
                $installExitCode = $LASTEXITCODE
                if ($installExitCode -ne 0) {
                    throw "devcon install failed with exit code $installExitCode"
                }
            }
            else { Write-Verbose "Skipped: -WhatIf or -Confirm declined the install." }

            # The INF's [UMDriverCopy] ships only MttVDD.dll, so C:\VirtualDisplayDriver\vdd_settings.xml
            # would not exist after a DevCon-only install, and every runtime toggle fails because
            # UpdateXmlToggleSetting() opens that exact path read-write. Place it explicitly.
            $settingsSource = Join-Path (Split-Path $driverInf -Parent) 'vdd_settings.xml'
            if (-not (Test-Path $settingsSource)) {
                $foundSettings = Get-ChildItem -Path $tempDir -Filter 'vdd_settings.xml' -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($foundSettings) { $settingsSource = $foundSettings.FullName }
            }
            if (-not (Test-Path $settingsSource)) {
                throw "vdd_settings.xml not found in the extracted driver package under $tempDir. The driver would start with no runtime configuration."
            }
            $settingsTargetDir = Join-Path $env:SystemDrive 'VirtualDisplayDriver'
            if (-not (Test-Path $settingsTargetDir)) {
                New-Item -ItemType Directory -Path $settingsTargetDir -Force | Out-Null
            }
            Copy-Item -Path $settingsSource -Destination (Join-Path $settingsTargetDir 'vdd_settings.xml') -Force
            Write-Log -Message "Runtime configuration installed at $settingsTargetDir\vdd_settings.xml"
        }
        else { # Action must be 'uninstall'
            Write-Log -Message "Starting driver uninstallation..."
            if (Get-VirtualDisplayDevice) {
                Write-Verbose "Driver found. Preparing to remove."
                if ($PSCmdlet.ShouldProcess("Root\MttVDD", "Uninstall Virtual Display Driver")) {
                    & $devconExe remove "Root\MttVDD"
                    $removeExitCode = $LASTEXITCODE
                    if ($removeExitCode -ne 0) {
                        throw "devcon remove failed with exit code $removeExitCode"
                    }
                }
                else { Write-Verbose "Skipped: -WhatIf or -Confirm declined the removal." }
            }
            else { Write-Log -Message "Driver is not currently installed. Nothing to do." }
        }
    }
    elseif ($Action -in @('enable', 'disable', 'toggle')) {
        $device = Get-VirtualDisplayDevice
        # This used to only log a warning and fall through to the unconditional "completed
        # successfully" at the end of the try, so `disable` against a driver that is not installed
        # printed both a warning and a success line and exited 0. A caller driving this script
        # through -Json/-Silent has no other way to tell the two apart.
        if (-not $device) { throw "Device not found. Cannot perform '$Action'. Please install the driver first." }
        elseif ($Action -eq 'enable') {
            if ($PSCmdlet.ShouldProcess($device.FriendlyName, "Enable Virtual Display Driver")) {
                Write-Log -Message "Enabling device: $($device.FriendlyName)..."
                $device | Enable-PnpDevice -Confirm:$false
            }
            else { Write-Verbose "Skipped: -WhatIf or -Confirm declined the enable." }
        }
        elseif ($Action -eq 'disable') {
            if ($PSCmdlet.ShouldProcess($device.FriendlyName, "Disable Virtual Display Driver")) {
                Write-Log -Message "Disabling device: $($device.FriendlyName)..."
                $device | Disable-PnpDevice -Confirm:$false
            }
            else { Write-Verbose "Skipped: -WhatIf or -Confirm declined the disable." }
        }
        elseif ($Action -eq 'toggle') {
            $enabling = $device.Status -notin ('OK', 'Degraded', 'Started')
            $verb = if ($enabling) { 'Enable' } else { 'Disable' }
            if ($PSCmdlet.ShouldProcess($device.FriendlyName, "$verb Virtual Display Driver")) {
                if ($enabling) {
                    Write-Log -Message "Device is currently disabled or in an error state. Enabling..."
                    $device | Enable-PnpDevice -Confirm:$false
                }
                else {
                    Write-Log -Message "Device is currently enabled. Disabling..."
                    $device | Disable-PnpDevice -Confirm:$false
                }
            }
            else { Write-Verbose "Skipped: -WhatIf or -Confirm declined the toggle." }
        }
    }
    elseif ($Action -eq 'status') {
        Write-Log -Message "Checking driver status..."
        $device = Get-VirtualDisplayDevice

        $statusResult = [ordered]@{
            installed = $false
            status = "not installed"
        }

        if ($device) {
            $isEnabled = ($device.Status -in ('OK', 'Degraded', 'Started'))
            $statusText = if ($isEnabled) { "Enabled" } else { "Disabled" }
            
            $statusResult.installed = $true
            $statusResult.status = $statusText.ToLower()
            $statusResult.details = @{
                friendlyName = $device.FriendlyName
                instanceId   = $device.InstanceId
                pnpDeviceID  = $device.PNPDeviceID
            }
            $message = "Driver is installed. Status: $statusText."
        }
        else {
            $message = "Driver is not installed."
        }

        if ($Json.IsPresent) {
            # For 'status', the JSON object is the primary output, bypassing the standard log format.
            $statusResult | ConvertTo-Json -Compress | Write-Output
        }
        else {
            Write-Log -Message $message
        }
    }

    Write-Log -Message "Operation '$Action' completed successfully." -Status 'Success'
}
catch {
    # The 'catch' block will execute if any command in the 'try' block throws an error.
    $errorMessage = "An error occurred during the '$Action' operation: $($_.Exception.Message) `nAt line: $($_.InvocationInfo.ScriptLineNumber)"
    Write-Log -Message $errorMessage -Status 'Error'
    exit 1
}
finally {
    # This block ALWAYS runs, ensuring cleanup happens after success or failure.
    # If the user ran with -Verbose, we assume they are debugging.
    # We will NOT delete the temporary folder so they can inspect its contents.
    if ($PSBoundParameters.ContainsKey('Verbose')) {
        Write-Verbose "Verbose mode is active. Temporary directory will not be deleted so you can inspect its contents: $tempDir"
    }
    else {
        if (Test-Path $tempDir) {
            # This Write-Verbose message will not be visible without -Verbose, but is good practice.
            Write-Verbose "Cleaning up temporary directory: $tempDir"
            Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# Add a final pause unless in Silent or JSON mode so the user can see the output.
if (-not $Silent.IsPresent -and -not $Json.IsPresent) {
    Write-Host ""; Write-Host "Press any key to exit..." -ForegroundColor Gray
    [System.Console]::ReadKey($true) | Out-Null
}
