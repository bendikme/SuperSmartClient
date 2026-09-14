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

cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_VIEWER=ON -DENABLE_GNUTLS=ON -DENABLE_NETTLE=OFF \
  -DENABLE_H264=OFF -DENABLE_NLS=OFF
cmake --build build-windows --target vncviewer unifiedpanel dashboardmodel --parallel
ctest --test-dir build-windows/tests/unit -R 'UnifiedPanel|DashboardModel' --output-on-failure
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
python tests/integration/unified_panel.py --viewer build-windows/vncviewer/vncviewer.exe
python tests/integration/dashboard.py --viewer build-windows/vncviewer/vncviewer.exe
```

In PowerShell, from the repository root, create a portable ZIP:

```powershell
.\contrib\unified\package-windows.ps1 -BuildDir build-windows `
  -RuntimeBin C:\msys64\mingw64\bin
```

For a repeat package, supply a fresh `-OutputDir`. The script includes all
transitive MinGW DLL dependencies and the library license notices. It creates
`dist/SuperSmartClient-windows-x64.zip` and its SHA-256 checksum. The program is
not code-signed.

## Linux (Ubuntu)

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libfltk1.3-dev libgnutls28-dev libjpeg-turbo8-dev libpixman-1-dev \
  zlib1g-dev libgtest-dev libpam0g-dev libxdamage-dev libxfixes-dev \
  libxrandr-dev libxtst-dev libxi-dev libxinerama-dev libxcursor-dev \
  libxft-dev libpng-dev xvfb xauth xdotool openssl libsqlite3-dev \
  libsecret-1-dev libsecret-tools gnome-keyring dbus-x11
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_VIEWER=ON -DENABLE_GNUTLS=ON -DENABLE_NETTLE=OFF \
  -DENABLE_H264=OFF -DENABLE_WAYLAND=OFF -DENABLE_NLS=OFF
cmake --build build-linux --target vncviewer unifiedpanel dashboardmodel --parallel
ctest --test-dir build-linux/tests/unit -R UnifiedPanel --output-on-failure
xvfb-run -a python3 tests/integration/unified_panel.py \
  --viewer build-linux/vncviewer/vncviewer --input-tests
bash tests/integration/isolated-keyring.sh ctest --test-dir build-linux/tests/unit \
  -R DashboardModel --output-on-failure
bash tests/integration/isolated-keyring.sh xvfb-run -a -s '-screen 0 1600x1000x24' \
  python3 tests/integration/dashboard.py --viewer build-linux/vncviewer/vncviewer --input-tests
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
cmake --build build-linux --parallel
ctest --test-dir build-linux/tests/unit --output-on-failure
```
