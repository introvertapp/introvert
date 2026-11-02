# Installation

[← Documentation](../README.md#table-of-contents)

Download the app from [GitHub Releases](https://github.com/introvertapp/introvert/releases).
Choose the package for your operating system and processor under **Assets**.
Packaged releases include the required application runtimes; you do not need
Qt, FFmpeg, Python, or development tools.

Current releases do not have trusted publisher signatures. Use the workarounds
below only for packages you downloaded from this project's GitHub Releases.

## Choose a package

| System | Processor | Package | Minimum system |
| --- | --- | --- | --- |
| macOS | Apple Silicon | `macos-arm64.zip` | macOS 14.4 |
| macOS | Intel | `macos-x86_64.zip` | macOS 14.4 |
| Windows | Intel / AMD 64-bit | `windows-x86_64-setup.exe` | Windows 11 |
| Linux | Intel / AMD 64-bit | `linux-x86_64.deb` or `.rpm` | glibc 2.35; Ubuntu 22.04 or a compatible distribution |
| Linux | ARM64 | `linux-arm64.deb` or `.rpm` | glibc 2.39; Ubuntu 24.04 or a compatible distribution |

Package names begin with `Introvert-` and the version number. Windows ARM64
packages are not available. Linux needs a graphical desktop and compatible
system graphics libraries; having DEB or RPM support alone is not enough.

macOS runtime testing is recorded in the [compatibility table](../README.md#os-and-cpu-compatibility).
Native Windows and Linux runtime and installation testing is still pending.

## macOS

1. Download and open the ZIP for your Mac.
2. Move **Introvert.app** to **Applications**.
3. Open Introvert and allow access to your media folders or local network when prompted.

### If macOS blocks the app

1. Try opening Introvert once, then dismiss the warning.
2. Open **System Settings → Privacy & Security**.
3. Click **Open Anyway**, confirm, and open Introvert again.

This creates an exception for Introvert. See [Apple's instructions](https://support.apple.com/en-us/102445).

If the official download is still blocked by quarantine, quit Introvert, open
**Terminal**, and run:

```sh
xattr -dr com.apple.quarantine "/Applications/Introvert.app"
```

Open the app again. This removes the downloaded-file quarantine flag from
Introvert only. Adjust the path if you installed it elsewhere. If it still fails,
download a fresh copy rather than disabling Gatekeeper.

## Windows

1. Download and run the `windows-x86_64-setup.exe` installer.
2. Follow the installer, then open **Introvert** from the Start menu.
3. Allow network access if prompted and you plan to connect a media server.

Installation is for your user account.

### If Windows blocks the installer

For the **Windows protected your PC** SmartScreen warning, click **More info →
Run anyway**, then continue with installation. Microsoft describes this
[unsigned-app warning](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation).

If the downloaded file is blocked, right-click the installer, choose
**Properties**, select **Unblock** if shown, then **Apply** and run it again.
See [Microsoft's file-unblocking instructions](https://support.microsoft.com/en-us/windows/security/information-about-the-attachment-manager-in-microsoft-windows).

If **Run anyway** is unavailable, Smart App Control or an organization policy
may prevent installation. [Smart App Control has no per-app exception](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions);
on a managed PC, ask your administrator.

## Linux

Download the DEB or RPM for your processor. Open a terminal in the download
folder containing only the Introvert package you want to install.

On Ubuntu or a compatible Debian-based system:

```sh
sudo apt install ./Introvert-*.deb
```

On a compatible RPM system using DNF:

```sh
sudo dnf install ./Introvert-*.rpm
```

Launch **Introvert** from your desktop's application menu. Later application
updates may ask for system authorization through your package manager.

### If Linux rejects an unsigned RPM

If DNF refuses the official package specifically because it is unsigned, allow
unsigned local packages for this installation only:

```sh
sudo dnf --setopt=localpkg_gpgcheck=0 install ./Introvert-*.rpm
```

This [DNF option](https://dnf.readthedocs.io/en/latest/conf_ref.html)
leaves repository signature checks unchanged. A stricter system policy may still
require signed packages. Local DEB installation with `apt` normally needs no
signature workaround.

## Next step

[Connect your media](setup.md). Download a packaged application rather than
GitHub's **Source code** archives; the public source checkout does not include
the local build and packaging tools.
