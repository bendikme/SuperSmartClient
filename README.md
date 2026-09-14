# SuperSmartClient

A Windows VNC viewer for **Siemens SIMATIC HMI Unified Comfort panels**, forked
from [TigerVNC](https://github.com/TigerVNC/tigervnc) **v1.16.2**. The native
viewer uses TigerVNC's existing GnuTLS / VeNCrypt implementation. Siemens lists
TigerVNC as a compatible client in its
[Unified Comfort Panels V20 manual](https://docs.tia.siemens.cloud/r/unified_comfort_panels_enus_20/operating-the-control-panel/network-and-internet/remote-connection).

This fork adds a panel connection profile to the native viewer, including the
Windows application. It does not change the Java viewer or the VNC servers.
It is an independent project, not a Siemens product.

The fork also fixes an upstream `close` macro collision with current MinGW
headers, so the native Windows viewer builds with the current MSYS2 toolchain.

## Connect to a panel

1. On the panel, open **Control Panel > Network and Internet > Remote connection**.
   Configure the two different SmartServer passwords and enable **Smart Server**.
   The V20 manual specifies exactly eight characters per password, including
   uppercase, lowercase, a digit and a special character.
2. Leave **Secure communication via self-signed certificate** enabled. The default
   desktop access port is **5900**; use the panel's configured port if different.
3. Run `SuperSmartClient.exe`. Enter the panel IP address, or `hostname::port`,
   for example `192.168.0.10::5900`. IPv6 uses `[address]::port`.
4. Select **Unified panel - certificate TLS** (the default).
   Check **Monitor only** to suppress mouse and keyboard input. For control,
   leave it unchecked and use a password with the panel's remote-control right.
5. On first connection, verify the presented certificate with the panel's owner
   before accepting the exception. TigerVNC remembers accepted public keys and
   prompts if a key changes. Alternatively, configure a trusted certificate in
   **Options > Security > Path to X509 CA certificate**.
6. Enter the SmartServer password when prompted. This is separate from the
   panel's web or runtime user account.

Save reusable connection settings with **Save as...**. Passwords are not saved
in these configuration files.

## What the Unified profile does

| Setting | Behavior |
| --- | --- |
| Certificate TLS | Requires `X509Vnc`: TLS plus VNC password authentication |
| Anonymous TLS (legacy) | Explicitly selects `TLSVnc`; encrypts traffic but does not verify the panel identity |
| Authentication | Never falls back to plaintext or passwordless VNC |
| Shared access | Requests a shared session, so connecting does not request disconnection of other viewers |
| Panel resolution | Disables remote resizing and clears desktop-size requests |
| Clipboard | Disables clipboard exchange |
| Monitor / control | Uses the viewer's `ViewOnly` setting; panel permissions still apply |

The profile is applied after command-line or file settings, before each
connection and reconnect. Its enforced settings are disabled in the Options
dialog. Select **Standard VNC** (`-UnifiedPanel=0`) to use ordinary TigerVNC
settings; this stops enforcing the profile and keeps the current option values.

The exact TLS subtype offered by an individual panel depends on its firmware.
Use **Anonymous TLS (legacy)** only when the server is known to offer `TLSVnc`.
This mode is explicit; the viewer never silently downgrades certificate TLS.
Self-signed certificates use the upstream certificate verification and trust
prompts; this fork does not bypass verification or enable obsolete TLS versions.

```powershell
.\SuperSmartClient.exe -UnifiedPanel -ViewOnly 192.168.0.10::5900
.\SuperSmartClient.exe -UnifiedPanel -ViewOnly=0 panel.example::5900
.\SuperSmartClient.exe -UnifiedPanel -X509CA C:/certificates/panel.pem panel.example
.\SuperSmartClient.exe -UnifiedPanel -UnifiedSecurity=AnonymousTLS 192.168.0.10
```

## Compatibility and verification

The target is Unified Comfort panel SmartServer access, based on Siemens'
documentation. This is not WinCC Unified browser access, an embedded panel
application, or support for every classic Comfort / Basic panel firmware.

Automated tests exercise the real viewer against a local RFB / VeNCrypt server,
including certificate TLS, password authentication, framebuffer delivery and
rejection of unencrypted or passwordless servers. Unit tests cover the profile
policy and standard VNC behavior. **Physical Unified panel testing is still
required**; no panel model or firmware has yet been hardware-verified.

Local validation on 2026-09-14: Windows x64 and Linux builds passed, with 273
Linux unit tests, 6 Windows profile tests, and 10 simulated connection tests on
each platform. A separate build without GnuTLS passed both tests checking that
the Unified profile fails closed while ordinary VNC remains available.

If negotiation fails, check that SmartServer is enabled, the address/port are
correct and its encryption setting matches the selected TLS mode. Do not use
the HTTPS port 443 as the VNC port. If authentication fails, use one of the two
SmartServer passwords. Monitor-only access can also be imposed by the panel.

## Build and test

See [contrib/unified/BUILDING.md](contrib/unified/BUILDING.md) for Windows and
Linux builds and the automated checks. The portable Windows package contains
the viewer and its runtime DLLs; extract the whole folder before running it.

The original upstream documentation is in [README.rst](README.rst) and
[BUILDING.txt](BUILDING.txt). TigerVNC's license and copyright notices are
preserved in [LICENCE.TXT](LICENCE.TXT) and the original source files.
