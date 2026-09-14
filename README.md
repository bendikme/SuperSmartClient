# SuperSmartClient

A Windows VNC viewer for **Siemens SIMATIC HMI Unified Comfort panels**, forked
from [TigerVNC](https://github.com/TigerVNC/tigervnc) **v1.16.2**. The native
viewer uses GnuTLS for both Siemens' `VNC OVER SSL` transport and standard
VeNCrypt connections. Siemens lists
TigerVNC as a compatible client in its
[Unified Comfort Panels V20 manual](https://docs.tia.siemens.cloud/r/unified_comfort_panels_enus_20/operating-the-control-panel/network-and-internet/remote-connection).

The native application includes a compact dashboard with multiple panel views,
automatic reconnect, saved passwords, named layouts and portable database
export/import. It also retains TigerVNC's single-connection viewer and Siemens
TLS negotiation. The Java viewer and VNC servers are unchanged.
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
3. Run `SuperSmartClient.exe` and choose **Add panel**. Enter a name and the panel IP address, or `hostname::port`,
   for example `192.168.0.10::5900`. IPv6 uses `[address]::port`.
4. Select **Unified - certificate TLS** (the default) and enter the SmartServer password.
   Leave **Monitor only** unchecked for mouse and keyboard control, or check it
   for viewing without control. This choice is independent of encryption and
   works with both TLS modes. The panel must also grant remote-control rights.
5. On first connection, verify the presented certificate with the panel's owner
   before accepting the exception. TigerVNC remembers accepted public keys and
   prompts if a key changes. Alternatively, configure a trusted certificate in
   the `-X509CA C:/certificates/panel.pem` command-line option.
6. Leave **Remember password**, **Reconnect automatically** and **Connect on startup**
   checked as needed, then choose **Save & connect**. The SmartServer password is
   separate from the panel's web or runtime user account.

## Panel grid and saved layouts

- View up to **32 panels** in one window with **1–4 grid columns**. Each panel has
  its own TLS connection, credentials, framebuffer and reconnect state.
- Drag a panel's header to reorder it. Drag the thin dividers between grid panes
  to resize adjacent panes; double-click a divider to reset the proportions.
  The **...** menu also offers **Move earlier** and **Move later**.
  Resizing a view never changes the HMI's resolution.
- The arrangement dropdown offers a single column, side-by-side, stacked,
  top-and-two-below, left-and-two-right, and 2-by-2, 3-by-2 and 3-by-3 grids.
  Extra panels continue below. **Free placement** restores independent saved
  window positions and sizes; drag a header to move a window or its corner
  to resize it. Selecting an overlapping window brings it to the front.
- Grids fill the available workspace and retain their divider proportions when
  the application resizes. Connecting, reconnecting or changing a panel's
  resolution does not move grid boundaries. Pictures keep their aspect ratio.
- The toolbar uses one 40-pixel row. Panel headers are 24 pixels high, with
  one-pixel borders and four-pixel gaps; there are no panel footers.
- Double-click a header, or use its **[ ]** button, to focus one panel.
  **Overview** restores the arrangement while other connections remain active.
- **Full screen** or **F11** fills the display with panel pictures, hiding the
  toolbar and all panel chrome. A small **x** button in the top-right corner,
  **F11**, or **Esc** restores the window. These shortcuts stay local. Fullscreen
  temporarily fits the pictures without changing saved scaling, positions or
  divider proportions. All connections continue streaming.
- Use the **Control / Monitor** button on each panel to choose its input mode.
  A panel's **...** menu provides editing, reconnect, refresh and removal.
- Choose **Layouts > Save layout as...** to name a complete arrangement.
  **New empty layout** starts another group. Up to **100 layouts** are supported.
  Changes to the current layout are saved automatically.
- Selecting a name in the **Layout dropdown** opens and connects the entire
  group. Connections in the previous layout close. On app startup, the last
  active layout reopens, respecting each panel's **Connect on startup** setting.
- The toolbar **...** menu contains **Connect all**, **Disconnect all**,
  **Reset pane divisions**, **Light theme / Dark theme**, and custom column counts.
- Roboto regular, bold and italic fonts are bundled and used throughout the
  application UI. They are loaded privately for this app, without installing
  fonts into Windows or changing the panel's own display.

Connection addresses, order, grid/free placement, individual scaling,
input/security modes and preferences
are stored in a SQLite application database at
`%LOCALAPPDATA%\SuperSmartClient\dashboard.db` on Windows, or
`$XDG_CONFIG_HOME/supersmartclient/dashboard.db` on Linux (normally under
`~/.config`). `-DashboardConfig path/to/another.db` opens a separate database.

Passwords are protected by the current Windows account using
[Windows DPAPI](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata).
On Linux, builds with [libsecret](https://gnome.pages.gitlab.gnome.org/libsecret/)
use the desktop keyring. There is no plaintext-password fallback. Unchecking
**Remember password** retains it only for the current application session.
Lost connections retry after 1, 2, 4, 8, 16 and then 30 seconds. Missing or
rejected passwords and rejected certificates pause reconnect until corrected;
manually disconnecting also stops retries.

### Individual window size and scaling

Open a panel's **... > Size and scale...** menu. Choose **Automatic** to use
the resolution reported by the panel, or one of these Unified Comfort sizes:

| Model | Display | Native resolution |
| --- | --- | --- |
| MTP700 | 7 inch | 800 x 480 |
| MTP1000 | 10.1 inch | 1280 x 800 |
| MTP1200 | 12.1 inch | 1280 x 800 |
| MTP1500 | 15.6 inch | 1366 x 768 |
| MTP1900 | 18.5 inch | 1920 x 1080 |
| MTP2200 | 21.5 inch | 1920 x 1080 |

Resolutions follow Siemens' V20 technical specifications for
[MTP700/1000/1200](https://docs.tia.siemens.cloud/r/unified_comfort_panels_enus_20/technical-information/technical-specifications/mtp700-mtp1000-mtp1200-unified-comfort)
and [MTP1500/1900/2200](https://docs.tia.siemens.cloud/r/unified_comfort_panels_enus_20/technical-information/technical-specifications/mtp1500-mtp1900-mtp2200-unified-comfort).

Uncheck **Fit picture to the window** and set **Scale (%)**, from 10 to 200.
For example, MTP1200 at 50% gives a **640 x 400 picture area**, with the compact
header and border added outside it. Each panel keeps its own percentage and
screen preset. Fixed sizes switch the workspace to free placement so grid
cells do not override the requested dimensions. Dragging a corner enables Fit
for that panel. Grids fit the pictures while preserving each free window's
scale and geometry. Focused and fullscreen views temporarily fit the available
area; the saved scale returns when you leave those views.

All positions, sizes, percentages and screen presets survive restart, layout
selection and database export/import. Scaling is local to the viewer. The
HMI framebuffer remains unchanged, and control coordinates map back to its
actual resolution. Pictures keep their aspect ratio if a selected screen
preset differs from the connected panel.

### Export and import

**Layouts... > Export database...** exports every named layout to a `.sscdb` file.
With **Include saved passwords** checked, supply an export password of at least
eight characters. The complete portable database is encrypted and authenticated
using AES-256-GCM with a random salt/nonce and PBKDF2-HMAC-SHA256 (600,000
iterations), through the [GnuTLS cryptographic API](https://gnutls.org/manual/html_node/Cryptographic-API.html).
The export password is required on import. No plaintext credential database is
written to disk during export or import.

Uncheck **Include saved passwords** for a regular SQLite export containing
layouts and connection settings only. Passwords must then be entered on the
destination PC. Copying the live `.db` directly also does not make account-bound
passwords portable; use the protected export to transfer them.

**Import database...** adds layouts to the existing library, preserving current
layouts and resolving duplicate names with a numbered suffix. Protected exports
can move between Windows and Linux; imported passwords are protected again by
the destination account when saved. Choose an imported layout from the dropdown
to connect. Certificate trust decisions remain local to each PC.

### Single-connection viewer

Passing a server address on the command line opens the original viewer. Use
`SuperSmartClient.exe -Dashboard=0` for its connection dialog. Its **Save as...**
files retain the original TigerVNC behavior and do not save passwords. The
dashboard database is separate from these files and the legacy viewer settings.

## What the Unified profile does

| Setting | Behavior |
| --- | --- |
| Certificate TLS | Detects Siemens `VNC OVER SSL`, or uses VeNCrypt `X509Vnc`; both require TLS and VNC password authentication |
| Anonymous TLS (legacy) | Explicitly selects `TLSVnc`; encrypts traffic but does not verify the panel identity |
| Authentication | Never falls back to plaintext or passwordless VNC |
| Shared access | Requests a shared session, so connecting does not request disconnection of other viewers |
| Panel resolution | Disables remote resizing and clears desktop-size requests |
| Clipboard | Disables clipboard exchange |
| Input | Respects `ViewOnly`: unchecked allows control; checked suppresses mouse and keyboard input |

In the single-connection viewer, the profile is applied after command-line or file settings, before each
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
.\SuperSmartClient.exe -UnifiedPanel -ViewOnly=0 192.168.0.10::5900
.\SuperSmartClient.exe -UnifiedPanel -X509CA C:/certificates/panel.pem panel.example
.\SuperSmartClient.exe -UnifiedPanel -UnifiedSecurity=AnonymousTLS 192.168.0.10
```

`ViewOnly=0` enables control and `ViewOnly=1` enables monitoring in connection
files or on the command line. You can also change this in **Options > Input**
during a connection. Settings saved by an earlier build may still have
`ViewOnly=1`; uncheck **Monitor only** or override it with `-ViewOnly=0`.

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
Unit tests verify that both TLS modes preserve the selected input mode.
Additional Linux tests send mouse/key events only to a fixture viewer inside
an isolated Xvfb display. They verify encrypted control input is delivered and
monitor-only input is suppressed for Siemens TLS and both VeNCrypt TLS modes.
Dashboard tests cover multiple simultaneous panels with different stored
passwords, independent reconnect, application restart, paused authentication
failures, grid reordering/resizing, layout selection, the Add panel form and
separate control/monitor input. They also verify layout presets, independent
scaling, free placement, restored view sizes and scaled input coordinates.
Database tests cover persistence, merging,
credential isolation, portable export/import and rejection of damaged or
incorrectly password-protected exports. All dashboard network tests use local
simulators; no physical panel input is sent.

Local validation includes simulated connections on both platforms, profile
unit tests and the Linux input tests. A separate build without GnuTLS
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
