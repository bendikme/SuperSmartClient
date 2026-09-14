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
  mingw-w64-x86_64-pixman mingw-w64-x86_64-gtest

cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_VIEWER=ON -DENABLE_GNUTLS=ON -DENABLE_NETTLE=OFF \
  -DENABLE_H264=OFF -DENABLE_NLS=OFF
cmake --build build-windows --target vncviewer unifiedpanel --parallel
ctest --test-dir build-windows/tests/unit -R UnifiedPanel --output-on-failure
```

FLTK **1.3** is required by the upstream release. RSA-AES and H.264 are optional
and disabled here; they are not required for the Unified TLS profile.

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
  libxft-dev libpng-dev xvfb xauth openssl
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_VIEWER=ON -DENABLE_GNUTLS=ON -DENABLE_NETTLE=OFF \
  -DENABLE_H264=OFF -DENABLE_WAYLAND=OFF -DENABLE_NLS=OFF
cmake --build build-linux --target vncviewer unifiedpanel --parallel
ctest --test-dir build-linux/tests/unit -R UnifiedPanel --output-on-failure
xvfb-run -a python3 tests/integration/unified_panel.py \
  --viewer build-linux/vncviewer/vncviewer
```

The integration tests start a local server on an ephemeral loopback port and
generate temporary test certificates. They run the compiled viewer through a
real TLS handshake, verify its VNC challenge response and shared-session flag,
send a framebuffer and check that the viewer continues requesting updates.
They cover both the Siemens `VNC OVER SSL` transport (TLS before the client RFB
reply) and VeNCrypt, fragmented greetings, certificate decisions, conflicting
command-line/file settings, rejected plaintext/passwordless offers and ordinary
VNC when the profile is disabled. All test connections use loopback addresses.
Profile tests also verify that the protocol writer suppresses mouse and
keyboard messages, including extended events, in monitor-only connections.

To run the complete upstream unit suite, build all the test targets first:

```sh
cmake --build build-linux --parallel
ctest --test-dir build-linux/tests/unit --output-on-failure
```
