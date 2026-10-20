# Virtual Display Driver — hardened fork

This is a fork of [VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver),
carrying a line-by-line review of the driver, the PowerShell helpers and the
build configuration.

Everything here was found by reading the code, not by running it. Where a
defect could be reproduced in isolation, there is a harness for it; where it
could not, the reasoning is in the commit message.

**Nothing in this fork has been run against a real display.** The driver was
built in all four configurations and the code paths were replayed in
isolation, but no one has installed it and watched a virtual monitor appear.
That is the gap between "the code looks right" and "the driver works", and it
is the first thing to close.

## What is here

The work is split into fifteen commits, each one a theme, and each one builds
on its own:

| # | commit | what it fixes |
|---|---|---|
| 1 | `check the two return values Code Analysis flags` | `AvTaskHandle` may be `NULL`; `CoCreateGuid` failure was discarded |
| 2 | `guard shared state and add the checked parse helpers` | unsynchronised shared state; `std::stoi` throwing out of a UMDF callback |
| 3 | `read each setting from the right source and check its type` | four registry readers that did not check the value type |
| 4 | `stop treating the absent-key sentinel as a real value` | `-1` and `0.0` used as real values; compiled defaults disagreeing with the XML |
| 5 | `store, gate and expose the HDR metadata and gamma ramp` | `std::map` caches read while written; a template outliving its monitor |
| 6 | `bound, deduplicate and physically plausible the mode list` | `pixelRate` wrapping; the preferred mode sorted away; rates no mode reports |
| 7 | `cache the log handle and stop dropping XML attributes` | a file opened per log line; rewriters that rewrote the wrong element |
| 8 | `hand the monitor a valid container ID and stop the frame loop hanging` | `GUID_NULL` handed to IddCx; a wait list read by index |
| 9 | `the mode buffer was sized from the wrong count` | a heap overflow when the mode list is longer than the buffer |
| 10 | `range-check the settings that were never range-checked` | a gamma of `0.0` passing validation; a ceiling below the shipped modes |
| 11 | `key pipe writes to Administrators and bound the waits` | any unelevated process could write commands to the driver |
| 12 | `drop the configurations the WDK cannot build, and fix the INF` | four configurations that never built; an INF token that drifted |
| 13 | `config: make option.txt and vdd_settings.xml match what the driver reports` | rates and resolutions the driver does not offer |
| 14 | `fix(scripts): the helpers could not do the job they were shipped to do` | six helpers filtering on a display name the driver never produces |
| 15 | `build: build Debug, pick the SDK numerically, and stop tracking build outputs` | Debug never built; the SDK picked as a string; committed `.exe` files |

The branch `fix/vdd-hardened` merges them in order.

## Three findings that were wrong

Reviewing the code turned up claims that did not survive checking, and acting
on them would have made things worse. They are recorded here because a review
that only lists what it fixed is not a review.

- **`IddCxMonitorSetHdrMetadata` does not exist.** It has zero occurrences
  across `iddcx.h` 1.0 through 1.10. The monitor surface is `Arrival`,
  `Create`, `Departure`, `GetSrmListVersion`, `QueryHardwareCursor{,2,3}`,
  `SetSrmList`, `SetupHardwareCursor`, `UpdateModes{,2}` — nothing else.
  `IddCx.h` describes the default-HDR event as *called by the OS to provide
  the metadata*, and `pHdr10` as *metadata a driver is to save*. Storing it
  in `g_HdrMetadataStore` is the implementation, not a stub.
- **The gamma callback should stay unregistered.** Registering it in
  `IDD_CX_CLIENT_CONFIG` declares gamma support, and
  `EndPointDiagnostics.GammaSupport` is `IDDCX_FEATURE_IMPLEMENTATION_NONE`
  with no ramp ever programmed.
- **Comparing a value against its default does not detect that a user
  changed it.** Four sites did this. `vdd_settings.xml` ships the same
  values, so three comparisons were always true and the fourth never was.

## Building

```powershell
$env:CL = "/wd6553 /wd28193 /wd6101 /wd28726 /wd6392 /wd28197"
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "Virtual Display Driver (HDR)\MttVDD.sln" /m /t:Build `
  /p:Configuration=Release /p:Platform=x64 /p:VisualStudioVersion=17.0 /p:SkipPackageVerification=true
```

Swap `Release`/`Debug` and `x64`/`ARM64` for the other three. `VisualStudioVersion=17.0`
is needed on VS 18, because the WDK only ships `Microsoft.DriverKit.Build.Tasks.17.0.dll`.

`CL` suppresses warnings that MSVC 18 raises on SDK headers. They are not
ours and the WDK forces `TreatWarningAsError` on SDK headers too.

## Reviewing this

The commits are ordered so that each one is self-contained, and the
dependency is explicit in the history:

```
T01  concurrency and parsing helpers   <- everything else depends on this
T05  metadata caches                   <- T16, T18, T20
T12  defaults and sentinels            <- T20
T17  mode buffer                       <- T18
```

A reviewer with the IddCx headers open can check any single commit. A reviewer
without them can still check the registry readers (#3), the defaults (#4), the
mode arithmetic (#6, #9) and the pipe ACL (#11) — those need only the
Windows API.

## What has not been done

- No hardware testing. See the note at the top.
- The four `.github/ISSUE_TEMPLATE/*.yml` files are deliberately unchanged:
  their diffs auto-assign the upstream maintainers, and triage policy is not
  a fork's to set.
- `AGENTS.md` and the audit reports are not in the fork. They are working
  notes, not code.
