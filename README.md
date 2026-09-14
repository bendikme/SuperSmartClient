# SuperSmartClient

A Windows VNC viewer for **Siemens SIMATIC HMI Unified Comfort panels**, forked
from [TigerVNC](https://github.com/TigerVNC/tigervnc) **v1.16.2**. The native
viewer uses GnuTLS for both Siemens' `VNC OVER SSL` transport and standard
VeNCrypt connections. Siemens lists
TigerVNC as a compatible client in its
[Unified Comfort Panels V20 manual](https://docs.tia.siemens.cloud/r/unified_comfort_panels_enus_20/operating-the-control-panel/network-and-internet/remote-connection).

This fork adds a monitor-only panel profile and Siemens TLS negotiation to the
native viewer, including the Windows application. It does not change the Java
viewer or the VNC servers.
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
   **Monitor only** is enforced in the Unified profile: mouse and keyboard
   messages are blocked in both the interface and the protocol writer.
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
| Certificate TLS | Detects Siemens `VNC OVER SSL`, or uses VeNCrypt `X509Vnc`; both require TLS and VNC password authentication |
| Anonymous TLS (legacy) | Explicitly selects `TLSVnc`; encrypts traffic but does not verify the panel identity |
| Authentication | Never falls back to plaintext or passwordless VNC |
| Shared access | Requests a shared session, so connecting does not request disconnection of other viewers |
| Panel resolution | Disables remote resizing and clears desktop-size requests |
| Clipboard | Disables clipboard exchange |
| Input | Enforces `ViewOnly` and blocks all pointer and keyboard messages at the protocol writer, including extended events |

The profile is applied after command-line or file settings, before each
connection and reconnect. Its enforced settings are disabled in the Options
dialog. Select **Standard VNC** (`-UnifiedPanel=0`) to use ordinary TigerVNC
settings; this stops enforcing the profile and keeps the current option values.

The TLS negotiation offered by an individual panel depends on its firmware.
For the Siemens transport, the server sends `VNC OVER SSL` followed by its RFB
version in plaintext. The viewer establishes and verifies TLS before sending
its version reply, then selects VNC password authentication inside TLS. It does
not send a plaintext version reply, skip certificate checks or fall back after
a TLS failure. The certificate profile also supports ordinary VeNCrypt servers.

Use **Anonymous TLS (legacy)** only when the server is known to offer `TLSVnc`.
This mode is explicit; the viewer never silently downgrades certificate TLS.
Self-signed certificates use the upstream certificate verification and trust
prompts; this fork does not bypass verification or enable obsolete TLS versions.

```powershell
.\SuperSmartClient.exe -UnifiedPanel -ViewOnly 192.168.0.10::5900
.\SuperSmartClient.exe -UnifiedPanel -X509CA C:/certificates/panel.pem panel.example
.\SuperSmartClient.exe -UnifiedPanel -UnifiedSecurity=AnonymousTLS 192.168.0.10
```

`ViewOnly=0` in a file or command line cannot enable input in the Unified
profile. Standard VNC retains upstream behavior; it is not a panel control mode.

## Compatibility and verification

The target is Unified Comfort panel SmartServer access, based on Siemens'
documentation. This is not WinCC Unified browser access, an embedded panel
application, or support for every classic Comfort / Basic panel firmware.

On 2026-09-14, a **Unified HMI TP1200 running firmware 20.2** was checked up to
its authentication offer. It used the Siemens greeting, TLS 1.3 with
`TLS_AES_256_GCM_SHA384`, and offered Tight (16) and VncAuth (2). The presented
certificate matched the thumbprint shown by Siemens Sm@rtClient. No login,
mouse events or keyboard events were sent. **Authenticated display access on
the physical panel has not been verified.**
The patched native GnuTLS connection code also completed TLS 1.3 with this
panel and stopped at the certificate decision without accepting the certificate.

Automated tests exercise the real Windows and Linux viewers against loopback
Siemens TLS and VeNCrypt servers. They cover TLS 1.2/1.3, fragmented greetings,
certificate decisions, password authentication, framebuffer delivery and
rejection of invalid greetings, plaintext/passwordless offers and TLS failures.
Unit tests verify the monitor-only policy and suppression of standard and
extended mouse/keyboard messages while allowing display updates.

Local validation: 20 simulated connection tests on each platform, 8 Windows
profile/input tests, and 275 Linux unit tests. A separate build without GnuTLS
checks that the Unified profile fails closed while ordinary VNC remains
available.

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
