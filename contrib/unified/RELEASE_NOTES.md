SuperSmartClient brings Siemens Unified panels together in one compact workspace.

## New in 1.0.1

- **Accept unknown certificates.** A new option in the workspace **...** menu
  accepts panel certificates without asking: new, changed, expired, not yet
  valid, weakly signed or issued for another hostname. It is off by default, asks
  for confirmation once when you turn it on, applies to every layout and is
  remembered between sessions. Connections stay encrypted, but the panel's
  identity is no longer checked, so use it only on a trusted network. The setting
  stays on this PC: it is never written to an exported database, and importing a
  database cannot turn it on.
- **Dropdown lists open where you expect them.** The layout, arrangement,
  connection security and Siemens screen size lists now open directly below their
  field, or above it when there is no room, instead of starting partly outside
  the screen and scrolling back into view.
- The workspace **...** menu now lines up with the right edge of its button
  instead of extending past the window.

## Updating

From 1.0.0, choose **Updates...** in the workspace **...** menu, then **Download
update** and **Install and restart**. Downloads and files are verified before
installation, with a backup for rollback. Saved layouts, passwords and accepted
certificates are kept.

For a new installation, download the Windows x64 ZIP, extract the entire folder,
and run `SuperSmartClient.exe`. Keep the included DLLs, fonts, manifest and update
helper beside the app. Saved layouts and credentials remain in your Windows user
profile.

## Features

- Siemens certificate and anonymous TLS, with independent Control/Monitor modes.
- Multiple panels, saved layouts, adjustable grids and free placement.
- Per-panel scaling, Siemens screen sizes and high-quality image filtering.
- Automatic reconnect and passwords protected by your Windows account.
- Export and import complete layout libraries.
- Roboto, light and dark themes, readable checkboxes and fullscreen viewing.
- Automatic daily release checks with verified in-app updates.

Requires Windows 10/11 x64 and Windows PowerShell 5.1. The portable application is
not code-signed. Linux can be built from source; in-app installation currently
supports the Windows portable package.
