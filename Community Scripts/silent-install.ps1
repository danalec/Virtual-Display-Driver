# Run this script in a powershell with administrator rights (run as administrator)
[CmdletBinding()]
param(
    # Latest stable version of NefCon installer
    [Parameter(Mandatory=$false)]
    [string]$NefConURL = "https://github.com/nefarius/nefcon/releases/download/v1.14.0/nefcon_v1.14.0.zip",

    # Latest stable version of VDD driver only.
    # Left empty on purpose: which asset to install depends on this machine's
    # architecture, and the upstream release names its AMD64 asset "x86".
    # Resolve-DriverArtifact below fills these in when they are not supplied.
    [Parameter(Mandatory=$false)]
    [string]$DriverURL = "",

    # SHA-256 of $NefConURL, as published in the release metadata of nefarius/nefcon v1.14.0.
    # The installer refuses to run if the download does not match.
    [Parameter(Mandatory=$false)]
    [string]$NefConSHA256 = "a15557da24a9efca203158de3b43b0eaf982db231f0194031f1ed428bc13e669",

    # SHA-256 of $DriverURL. Empty on purpose for custom URLs: the script then aborts
    # instead of installing something it cannot vouch for.
    [Parameter(Mandatory=$false)]
    [string]$DriverSHA256 = ""
);

$ErrorActionPreference = 'Stop'
# Version 1.0 on purpose, not Latest: it enforces one rule -- an unassigned variable
# raises instead of silently binding $null -- and that is exactly the rule that
# catches the $rate class of bug. An AST sweep of all 12 helpers found 0 uninitialized
# reads, so enabling it cannot regress anything that works today. Do NOT raise to
# 2.0/Latest without auditing property access first: 2.0 also throws on a missing
# property, and $monitor.LogicalDisplay.DeviceName in get_disp_num.ps1 does.
Set-StrictMode -Version 1.0;

# SHA-256 of each VirtualDisplayDriver-<arch>.Driver.Only.zip in release 25.7.23, as
# published by the GitHub releases API. Both were verified against the real downloads.
#
# The "x86" name is upstream's, not ours: unpacking that asset yields an MttVDD.dll
# whose PE machine type is 0x8664 (AMD64), which is what x64 machines need. ARM64 is a
# separate asset. This script used to hardcode the x86 URL, so an ARM64 machine silently
# got an AMD64 DLL it cannot load.
$pinnedDriverArtifacts = @{
    'X64' = @{
        Url    = 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip'
        Sha256 = 'e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a'
    }
    'Arm64' = @{
        Url    = 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-ARM64.Driver.Only.zip'
        Sha256 = 'dcd487161469c726bab60f174a7e7f70ee02afd437cad0d5c0e6755953de5a25'
    }
}

function Resolve-DriverArtifact {
    param([string]$Url, [string]$Sha256)

    if ($Url) { return @{ Url = $Url; Sha256 = $Sha256 } }

    # OSArchitecture, not Win32_Processor.Architecture: on ARM64 Windows running an
    # x64-emulated PowerShell the latter can report x64 while the OS is still ARM64,
    # which would select the AMD64 asset for an ARM64 machine.
    $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    $key = switch ($arch) {
        'Arm64' { 'Arm64' }
        'X64'   { 'X64' }
        default { throw "No pinned Virtual Display Driver package for architecture '$arch'. Pass -DriverURL and -DriverSHA256 explicitly." }
    }

    Write-Host "Detected architecture: $arch" -ForegroundColor Cyan
    return $pinnedDriverArtifacts[$key]
}

function Assert-Sha256 {
    param([string]$Path, [string]$Expected, [string]$Label)

    if ([string]::IsNullOrWhiteSpace($Expected)) {
        throw "No expected SHA-256 configured for ${Label}. Refusing to run an unverified download. Pass -${Label}SHA256 <hash> after verifying the artifact out of band."
    }
    if (-not (Test-Path $Path)) {
        throw "Downloaded file not found: $Path"
    }
    $actual = (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Expected.ToLowerInvariant()) {
        throw "SHA-256 mismatch for ${Label}: expected $Expected, got $actual. Aborting."
    }
    Write-Host "SHA-256 verified: $Label" -ForegroundColor DarkGreen
}

$driverArtifact = Resolve-DriverArtifact -Url $DriverURL -Sha256 $DriverSHA256
$DriverURL = $driverArtifact.Url
$DriverSHA256 = $driverArtifact.Sha256

# Create temp directory
# A unique name per run: a fixed "VDDInstall" collides between concurrent runs and
# between a run and the leftovers of a previous failed one.
$tempDir = Join-Path $env:TEMP "VDDInstall-$(Get-Random)";
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null;

# Everything below runs inside the try so that a failure at any step still rolls
# back the imported certificates and removes the temporary directory.
$installedThumbprints = @();
try {
    # Download and unzip NefCon
    Write-Host "Downloading and extracting NefCon..." -ForegroundColor Cyan;
    $NefConZipPath = Join-Path $tempDir "nefcon.zip";
    Invoke-WebRequest -Uri $NefConURL -OutFile $NefConZipPath -UseBasicParsing -ErrorAction Stop;
    Assert-Sha256 -Path $NefConZipPath -Expected $NefConSHA256 -Label 'NefCon';
    Expand-Archive -Path $NefConZipPath -DestinationPath $tempDir -Force -ErrorAction Stop;
    # The NefCon package ships one nefconw.exe per architecture. Resolve-DriverArtifact above
    # picks the driver by RuntimeInformation.OSArchitecture rather than
    # Win32_Processor.Architecture, because on ARM64 under an x64-emulated PowerShell the latter
    # reports x64 for an ARM64 machine. This used to hardcode "x64\nefconw.exe", so the ARM64 path
    # downloaded an ARM64 driver and then ran the x64 installer under emulation. Use the same
    # key here so the two selections cannot disagree.
    $nefconDir = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
        'Arm64' { 'ARM64' }
        'X64'   { 'x64' }
        'X86'   { 'x86' }
        default { $null }
    }
    if (-not $nefconDir) {
        throw "No nefconw.exe directory known for this architecture. Pass the archive via -NefConURL pointing at a build for this host."
    }
    $NefConExe = Join-Path $tempDir "$nefconDir\nefconw.exe";
    if (-not (Test-Path $NefConExe)) {
        # Fall back to x64 rather than failing outright, and say so. x64 runs everywhere through
        # emulation; this only matters when the archive carries no native build for this host.
        $fallbackExe = Join-Path $tempDir 'x64\nefconw.exe';
        if (Test-Path $fallbackExe) {
            Write-Host "nefconw.exe not present under '$nefconDir'; falling back to the x64 build (runs under emulation)." -ForegroundColor Yellow;
            $NefConExe = $fallbackExe;
        }
        else {
            throw "nefconw.exe not found after extraction under either '$nefconDir\' or 'x64\' at: $tempDir"
        }
    }
    else {
        Write-Host "Using nefconw.exe for $nefconDir" -ForegroundColor DarkGray;
    }

    # Download and unzip VDD
    Write-Host "Downloading and extracting VDD..." -ForegroundColor Cyan;
    $driverZipPath = Join-Path $tempDir 'driver.zip';
    Invoke-WebRequest -Uri $DriverURL -OutFile $driverZipPath -UseBasicParsing -ErrorAction Stop;
    Assert-Sha256 -Path $driverZipPath -Expected $DriverSHA256 -Label 'Driver';
    Expand-Archive -Path $driverZipPath -DestinationPath $tempDir -Force -ErrorAction Stop;

    # Extract the SignPath certificates
    Write-Host "Extracting SignPath certificates..." -ForegroundColor Cyan;
    # Catalog name as declared by the INF: MttVDD.inf:12 -> CatalogFile=MttVDD.cat
    $catFile = Join-Path (Join-Path $tempDir 'VirtualDisplayDriver') 'MttVDD.cat';
    if (-not (Test-Path $catFile)) {
        # Fall back to any catalog shipped in the archive, wherever it sits.
        $foundCat = Get-ChildItem -Path $tempDir -Filter 'MttVDD.cat' -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1;
        if (-not $foundCat) { throw "MttVDD.cat not found in the extracted driver package under $tempDir." }
        $catFile = $foundCat.FullName;
    }
    $catBytes = [System.IO.File]::ReadAllBytes($catFile);
    $certificates = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2Collection;
    $certificates.Import($catBytes);

    # Create the temp directory for certificates
    $certsFolder = Join-Path $tempDir "ExportedCerts";
    New-Item -ItemType Directory -Path $certsFolder -Force | Out-Null;

    # Write and store the driver certificates on local machine.
    # Each thumbprint is tracked so a failed install does not leave TrustedPublisher
    # populated: those entries require certmgr.msc to remove by hand.
    #
    # Only the ones this run actually added. The store is keyed by thumbprint, so
    # a certificate left there by an earlier successful run was recorded as ours
    # too, and the rollback deleted it -- leaving an installed driver without its
    # trusted publisher, which can stop it loading on the next reboot.
    Write-Host "Installing driver certificates on local machine." -ForegroundColor Cyan;
    $preExistingThumbprints = @(Get-ChildItem -Path "Cert:\LocalMachine\TrustedPublisher" -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Thumbprint);
    foreach ($cert in $certificates) {
        $certFilePath = Join-Path -Path $certsFolder -ChildPath "$($cert.Thumbprint).cer";
        # Set-Content -Encoding Byte was removed in PowerShell 7.
        [System.IO.File]::WriteAllBytes($certFilePath, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert));
        Import-Certificate -FilePath $certFilePath -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null;
        if ($preExistingThumbprints -notcontains $cert.Thumbprint) {
            $installedThumbprints += $cert.Thumbprint;
        }
    }

    # Install VDD
    Write-Host "Installing Virtual Display Driver silently..." -ForegroundColor Cyan;
    # The INF is expected in the VirtualDisplayDriver\ subfolder, but resolve it
    # rather than assume a layout, so a future repackage cannot silently break it.
    $driverInf = Join-Path (Join-Path $tempDir 'VirtualDisplayDriver') 'MttVDD.inf';
    if (-not (Test-Path $driverInf)) {
        $foundInf = Get-ChildItem -Path $tempDir -Filter 'MttVDD.inf' -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1;
        if (-not $foundInf) { throw "MttVDD.inf not found in the extracted driver package under $tempDir." }
        $driverInf = $foundInf.FullName;
    }
    Push-Location $tempDir;
    try {
        # nefconw.exe is the windowed build, a GUI-subsystem process. PowerShell's call
        # operator does not wait for one and never sets $LASTEXITCODE for it, so
        # $installExitCode was $null -- and $null -ne 0 is true, so the check below
        # failed every single time and the script rolled back an install that had
        # actually worked. Measured on a purpose-built GUI-subsystem executable:
        # $LASTEXITCODE stayed undefined after the call. Start-Process waits for the
        # process and reports its real exit code, whatever the subsystem is.
        $nefcon = Start-Process -FilePath $NefConExe -Wait -PassThru -NoNewWindow -ArgumentList @('install', ('"' + $driverInf + '"'), 'Root\MttVDD');
        $installExitCode = $nefcon.ExitCode;
    }
    finally {
        Pop-Location;
    }
    if ($installExitCode -ne 0) {
        throw "nefconw.exe install failed with exit code $installExitCode"
    }

    # Place the runtime config where the driver reads it. The INF's [UMDriverCopy] ships only
    # MttVDD.dll, so without this step C:\VirtualDisplayDriver\vdd_settings.xml never exists on a
    # silent-only install, and every runtime toggle the community scripts send over the pipe
    # fails: UpdateXmlToggleSetting() opens that exact path read-write. README promises the file
    # is at the default location.
    $settingsSource = Join-Path (Split-Path $driverInf -Parent) 'vdd_settings.xml';
    if (-not (Test-Path $settingsSource)) {
        $foundSettings = Get-ChildItem -Path $tempDir -Filter 'vdd_settings.xml' -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1;
        if ($foundSettings) { $settingsSource = $foundSettings.FullName }
    }
    if (-not (Test-Path $settingsSource)) {
        throw "vdd_settings.xml not found in the extracted driver package under $tempDir. The driver would start with no runtime configuration."
    }

    $settingsTargetDir = Join-Path $env:SystemDrive 'VirtualDisplayDriver';
    if (-not (Test-Path $settingsTargetDir)) {
        New-Item -ItemType Directory -Path $settingsTargetDir -Force | Out-Null;
    }
    Copy-Item -Path $settingsSource -Destination (Join-Path $settingsTargetDir 'vdd_settings.xml') -Force;
    Write-Host "Runtime configuration installed at $settingsTargetDir\vdd_settings.xml" -ForegroundColor Cyan;
}
catch {
    foreach ($thumbprint in $installedThumbprints) {
        Remove-Item -Path "Cert:\LocalMachine\TrustedPublisher\$thumbprint" -Force -ErrorAction SilentlyContinue;
    }
    Write-Host "Installation failed; rolled back $($installedThumbprints.Count) certificate(s)." -ForegroundColor Red;
    Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue;
    throw;
}

Write-Host "Driver installation completed." -ForegroundColor Green;
Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue;
