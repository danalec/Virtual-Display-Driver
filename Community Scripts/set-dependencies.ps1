$ErrorActionPreference = 'Stop';

# Define the repository name and URL
$repo = @{Name = "PSGallery"; Url = "https://www.powershellgallery.com/api/v2" }

# Define the requred modules and their required versions BEFORE using them
$modules = @(
    @{
        Name    = "DisplayConfig"
        Version = "1.1.1"
    },
    @{
        Name    = "MonitorConfig"
        Version = "1.0.3"
    }
)
Write-Output "resolving dependencies"

# Check if the repository is registered
if (-not (Get-PSRepository -Name $repo.Name -ErrorAction SilentlyContinue)) {
    Write-Host "PSRepository '$($repo.Name)' cannot be found. Make sure it is available."
    exit 1
}

# Trusting a repository is a persistent, user-scoped change, and this file is dot-sourced by
# seven helper scripts. So running `rotate-VDD.ps1 90` left PSGallery permanently Trusted, with
# nothing asked for, no confirmation, and no mention in the README. Apply the policy only for the
# duration of the installs below and put the original one back afterwards -- PowerShell runs a
# finally block even when the body calls exit, so the early `exit 1` paths restore it too.
#
# The original policy has to be read from the repository itself. $repo is a hashtable with Name
# and Url in it, so $repo.InstallationPolicy was $null, the `if ($originalPolicy)` below was
# never true, and PSGallery was left Trusted for the rest of the session.
$originalPolicy = (Get-PSRepository -Name $repo.Name -ErrorAction SilentlyContinue).InstallationPolicy
try {
    Set-PSRepository -Name $repo.Name -InstallationPolicy Trusted `
        -InformationAction Ignore -WarningAction SilentlyContinue

    # Loop through the modules and ensure they are imported & installed
    foreach ($m in $modules) {

    $requiredVersion = [version]$m.Version

    # Already imported at the required version? Then continue.
    # The version matters: a bare "Get-Module -Name" accepts any version, so an
    # already-loaded DisplayConfig 6.x would bypass the pin and change the
    # properties returned by Get-DisplayColorInfo.
    $imported = Get-Module -Name $m.Name | Where-Object { $_.Version -eq $requiredVersion }
    if ($imported) {
        Write-Output "$($m.Name) $($requiredVersion) is imported."
        continue
    }

    $loadedOther = Get-Module -Name $m.Name | Select-Object -First 1
    if ($loadedOther) {
        Write-Output "$($m.Name) $($loadedOther.Version) is imported but $($requiredVersion) is required: reload the pinned version."
    }

    # If the pinned version is available on disk then import that exact version
    $available = Get-Module -ListAvailable -Name $m.Name | Where-Object { $_.Version -eq $requiredVersion } | Select-Object -First 1
    if ($available) {
        Write-Output "$($m.Name) $($requiredVersion) is installed but not imported: import."
        Import-Module $m.Name -RequiredVersion $requiredVersion
    }
    else {
        # If the module is not on disk, check if it's available in the online gallery
        if (Find-Module -Name $m.Name -ErrorAction SilentlyContinue) {
            Write-Output "$($m.Name) is available online, but not installed or imported: install and import."
            # Install the module (specifying the required version)
            Install-Module -Name $m.Name -RequiredVersion $m.Version `
                -Force -Scope CurrentUser -AllowClobber

            # Import the module after installation
            Import-Module $m.Name -RequiredVersion $requiredVersion
        }
        else {
            # If the module cannot be found in the online gallery, abort the script.
            Write-Output "Module '$($m.Name)' is not imported, not available on disk, and not found in the online gallery. Cannot run the script."
            exit 1
        }
    }
    # Save Module for later use
    if (-not (Get-InstalledModule -Name $m.Name -MinimumVersion $m.Version -ErrorAction SilentlyContinue)) {
        Write-Output "Saving module $($m.Name)."
        Save-Module -Name $m.Name -Repository $repo.Name -RequiredVersion $m.Version `
            -Confirm:$false -InformationAction Ignore -WarningAction SilentlyContinue
    }
}
}
finally {
    if ($originalPolicy) {
        Set-PSRepository -Name $repo.Name -InstallationPolicy $originalPolicy `
            -InformationAction Ignore -WarningAction SilentlyContinue -ErrorAction SilentlyContinue
        Write-Output "Restored PSRepository '$($repo.Name)' installation policy to '$originalPolicy'."
    }
}

# Find the virtual display and fail loudly if it is not there.
#
# There is no fixed name to match on. The name Windows shows for the endpoint comes from
# AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName ("VirtualDisplayDriver Device") in
# Driver.cpp, and it can also be replaced by whatever the display stack reports for a monitor
# described by an EDID profile. The pattern below covers the names the driver and its INF can
# produce.
#
# Six helper scripts used to match "VDD by MTT", a string that appears nowhere in this repository
# and in no file it ships, so all six stopped at "Could not find the virtual display" with the
# driver installed and working. Match the names the driver can actually produce, and when none of
# them is present say what was found instead of repeating a guess.
#
# Extend the pattern here, not in the six callers, if a new EDID profile introduces a name
# outside this set.
function Get-VddDisplay {
    $all = @(Get-DisplayInfo -ErrorAction Stop)
    if ($all.Count -eq 0) {
        throw "DisplayConfig reported no displays at all."
    }

    $pattern = 'Virtual\s*Display\s*Driver|VirtualDisplayDriver|MttVDD|IDD\s*Sample|IddSampleDriver'
    $found = @($all | Where-Object { $_.DisplayName -and $_.DisplayName -match $pattern })

    if ($found.Count -eq 1) {
        return $found[0]
    }

    if ($found.Count -gt 1) {
        Write-Warning ("More than one display matches the virtual driver (" +
            (($found | ForEach-Object { $_.DisplayName }) -join ', ') + "). Using the first.")
        return $found[0]
    }

    $names = ($all | ForEach-Object { $_.DisplayName }) -join ', '
    throw ("Could not find the virtual display. Displays present: $names. " +
        "If the driver is installed, the name depends on the EDID profile selected in " +
        "vdd_settings.xml -- match it here rather than guessing another fixed string.")
}