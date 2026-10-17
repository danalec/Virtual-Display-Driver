# MttVDD

The indirect display driver project. For the supported installation path, see the
[repository README](../../README.md) — the easiest route is the Virtual Driver Control app.

## Manual installation (for experienced pros only)

1. Download the latest release as a zip file.
2. Open Device Manager, click on any device, then use the **Action** menu → **Add Legacy Hardware**.
3. Select "Add hardware from a list (Advanced)" and then **Display adapters**.
4. Click **Have Disk...** → **Browse...**, navigate to the extracted files and select `MttVDD.inf`.
5. Go to display settings to customize the resolution of the virtual displays.

## Note on the driver certificate

Earlier revisions of this file told you to "run the `*.bat` file to add the driver certificate as a
trusted root certificate". **There is no `.bat` in this folder**, and that step is no longer needed
for release builds: they are code-signed by [SignPath.io](https://signpath.io) with a certificate
from the [SignPath Foundation](https://signpath.org), so the driver is already trusted.

If you built the driver yourself with the WDK, it carries a self-signed test certificate and you do
need to install that certificate into `Local Machine` → **Trusted Root Certification Authorities**
yourself. Never commit certificates, private keys, or signing credentials to this repository.