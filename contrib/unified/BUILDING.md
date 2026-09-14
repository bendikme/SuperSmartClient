# Building SuperSmartClient

Build the native viewer with **GnuTLS enabled**. The upstream Java viewer is not
modified. See the root README for connection settings and verification limits.

## Windows x64

In an [MSYS2](https://www.msys2.org/) **MINGW64** shell:

```sh
pacman -Syu
# Restart the shell if the runtime update requests it, then run pacman -Syu again.
pacman -S --needed make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja mingw-w64-x86_64-fltk1.3 \
  mingw-w64-x86_64-libjpeg-turbo mingw-w64-x86_64-gnutls \
  mingw-w64-x86_64-pixman mingw-w64-x86_64-gtest mingw-w64-x86_64-sqlite3

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_VIEWER=ON -DENABLE_GNUTLS=ON -DENABLE_NETTLE=OFF \
  -DENABLE_H264=OFF -DENABLE_NLS=OFF
cmake --build build --target vncviewer unifiedpanel dashboardmodel --parallel
ctest --test-dir build/tests/unit -R 'UnifiedPanel|DashboardModel' --output-on-failure
```

FLTK **1.3** is required by the upstream release. RSA-AES and H.264 are optional
and disabled here; they are not required for the Unified TLS profile.
The dashboard requires C++17 and SQLite3. Windows password storage uses DPAPI.
The build copies the bundled Roboto fonts beside the executable. Keep the
`fonts` directory with the portable application; its license and source revision
are included. Linux loads these fonts privately through Fontconfig.

With the MinGW runtime directory on `PATH`, run both native integration suites
from PowerShell:

```powershell
python tests/integration/unified_panel.py --viewer build/vncviewer/vncviewer.exe
python tests/integration/dashboard.py --viewer build/vncviewer/vncviewer.exe
```

In PowerShell, from the repository root, create a portable ZIP:

```powershell
.\contrib\unified\package-windows.ps1 -BuildDir build `
  -RuntimeBin C:\msys64\mingw64\bin
```

Use `build/` as the only build directory in this checkout. Reuse the existing
cache for subsequent builds. Windows and Linux need separate toolchains; use
CI for the other platform, or clean `build/` before switching toolchains. Do not
rename a configured CMake tree. Keep logs and temporary diagnostics in `build/`.

The script includes all transitive MinGW DLL dependencies and library license
notices. It creates `dist/workspaces/SuperSmartClient-windows-x64.zip`, its
SHA-256 checksum, and the extracted package beside it. For a repeat package,
close the packaged app and add `-Replace` to the same command. Do not create a
fresh output directory for each change. The program is not code-signed.

If the app is running, use `-ZipOnly -Replace`. This stages files under
`build/package/` and refreshes the same ZIP without replacing the running
executable. Close the app before extracting that ZIP over its existing folder.

## Linux (Ubuntu)

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libfltk1.3-dev libgnutls28-dev libjpeg-turbo8-dev libpixman-1-dev \
  zlib1g-dev libgtest-dev libpam0g-dev libxdamage-dev libxfixes-dev \
  libxrandr-dev libxtst-dev libxi-dev libxinerama-dev libxcursor-dev \
  libxft-dev libpng-dev xvfb xauth xdotool openssl libsqlite3-dev \
  libsecret-1-dev libsecret-tools gnome-keyring dbus-x11
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_VIEWER=ON -DENABLE_GNUTLS=ON -DENABLE_NETTLE=OFF \
  -DENABLE_H264=OFF -DENABLE_WAYLAND=OFF -DENABLE_NLS=OFF
cmake --build build --target vncviewer unifiedpanel dashboardmodel --parallel
ctest --test-dir build/tests/unit -R UnifiedPanel --output-on-failure
xvfb-run -a python3 tests/integration/unified_panel.py \
  --viewer build/vncviewer/vncviewer --input-tests
bash tests/integration/isolated-keyring.sh ctest --test-dir build/tests/unit \
  -R DashboardModel --output-on-failure
bash tests/integration/isolated-keyring.sh xvfb-run -a -s '-screen 0 1600x1000x24' \
  python3 tests/integration/dashboard.py --viewer build/vncviewer/vncviewer --input-tests
```

The integration tests start a local server on an ephemeral loopback port and
generate temporary test certificates. They run the compiled viewer through a
real TLS handshake, verify its VNC challenge response and shared-session flag,
send a framebuffer and check that the viewer continues requesting updates.
They cover both the Siemens `VNC OVER SSL` transport (TLS before the client RFB
reply) and VeNCrypt, fragmented greetings, certificate decisions, conflicting
command-line/file settings, rejected plaintext/passwordless offers and ordinary
VNC when the profile is disabled. All test connections use loopback addresses.
Profile tests verify that encryption preserves the chosen input mode. Linux
`--input-tests` require Xvfb and xdotool and send events only to the child
viewer connected to the loopback fixture. They check mouse/key delivery in
control mode and suppression in monitor mode for all three TLS transports.

The dashboard suite checks simultaneous sessions, independent reconnect,
password restoration across restarts, local grid gestures, named layout
selection and the connection editor. Linux keyring tests use a temporary data
directory and their own D-Bus session; they do not use the desktop's real keyring.
If libsecret is unavailable at build time, Linux can still connect but cannot
persist passwords. A working desktop keyring is required at runtime.

To run the complete upstream unit suite, build all the test targets first:

```sh
cmake --build build --parallel
ctest --test-dir build/tests/unit --output-on-failure
```
