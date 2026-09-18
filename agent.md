# SuperSmartClient working instructions

## Build and output locations

- Use **`build/` as the only build directory in this checkout**. Configure with
  `cmake -S . -B build ...` and reuse it with `cmake --build build ...`.
- Do not create `build-windows`, `build-linux`, `build-no-tls`, dated builds,
  numbered builds, or separate directories for individual fixes.
- One CMake cache belongs to one toolchain. Do not rename a configured build
  tree or share its cache between Windows and WSL. Use CI for the other platform.
  If a local toolchain change is necessary, clean the generated `build/` tree
  and configure that same directory again.
- Keep build logs, test output, screenshots and temporary build diagnostics
  under `build/`, not in the repository root or `.tools/`.
- Keep only the current portable package in **`dist/workspaces/`**:
  `SuperSmartClient-windows-x64/`, its ZIP, and its SHA-256 file. Reuse this
  location with `package-windows.ps1 -Replace`; do not make output directories
  for each change. Close the app before replacing its package. Never terminate
  the user's running application to build, package or clean files.
- If the app is running, use `-ZipOnly -Replace` to stage the updated package
  under `build/package/` and refresh the ZIP in `dist/workspaces/`. Leave the
  running executable alone and explain that the ZIP must be extracted after
  closing the app.
- `.tools/msys64/` is the existing Windows toolchain. Reuse it; do not download
  another copy. Bootstrap archives, package downloads, extracted smoke-test
  copies and one-off diagnostic programs can be removed after use.
- Clean up temporary artifacts at the end of the task. Preserve source files,
  `.git`, licenses, fonts, the installed toolchain, and the current release.
  Never delete the user's application database, saved layouts or credentials.
- `contrib/unified/clean-workspace.ps1 -WhatIf` previews removal of known old
  artifacts. The script preserves `build/` and the current portable app. If
  automatic approval review blocks deletion, report that limitation; do not
  bypass it by changing tools or executing the same deletion through a script.
- Before recursive deletion or moving on Windows, resolve every target to an
  absolute path and verify it is inside the intended generated directory. Use
  PowerShell `-LiteralPath` operations throughout; never run blanket `git clean
  -fdx` or delete a directory containing a running executable.

## Panel input and verification

- **Never send mouse clicks or keyboard presses to the physical HMI.** The panel
  at `10.29.134.40:5900` is a Unified TP1200 running firmware 20.2. Do not run GUI
  automation against it or start a viewer using the user's saved live layouts
  as a test. This is a restriction on our testing, not a requirement to disable
  the application's control mode.
- Use the loopback fixtures in `tests/integration/` for connection tests. Input
  tests run only inside the verified isolated Xvfb display. Linux secret tests
  use `tests/integration/isolated-keyring.sh`, never the user's desktop keyring.
- Run checks relevant to the change. Documentation and cleanup do not require
  rebuilding the application. Do not create alternate builds just to repeat
  checks that already passed.
- Follow [contrib/unified/BUILDING.md](contrib/unified/BUILDING.md) for commands.
  Keep that guide, packaging defaults and CI paths consistent with these rules.

## Product requirements to preserve

- Siemens certificate TLS starts immediately after the `VNC OVER SSL` greeting,
  before the client RFB version reply. Preserve this handshake and certificate
  verification, plus standard VNC and anonymous TLS support. The dashboard's
  **Accept unknown certificates** setting is opt-in, stored only in the local
  database and never exported; it answers trust prompts, not the handshake.
- Encryption and Control/Monitor are independent per-panel settings. Keep
  connections, passwords, reconnect state and input focus isolated by panel.
- Preserve compact multi-panel layouts, named layout selection, automatic
  reconnect, protected password storage, database export/import, Siemens screen
  presets and independent local scaling. Scaling must not resize the HMI.
- Use the bundled Roboto fonts throughout the application UI and retain their
  license. Keep frames and spacing compact.

- Grid geometry must depend on workspace size, preset and saved divider ratios,
  never on framebuffer dimensions or connection state. Preserve free placement
  coordinates and independent scaling when switching arrangements.
- Keep the main toolbar to one compact row. Fullscreen hides toolbar and panel
  chrome, preserves windowed geometry, and retains a visible exit control.
  F11 and fullscreen Escape, including key releases, must remain local.
