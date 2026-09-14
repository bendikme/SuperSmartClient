#!/usr/bin/env python3
"""Exercise the native viewer against a loopback-only RFB/VeNCrypt fixture.

Requires Python 3, OpenSSL and a GUI display (use xvfb-run on Linux).
No panel, credentials, third-party Python packages or external servers required.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import contextlib
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import unittest


VIEWER = None
OPENSSL = "openssl"
PASSWORD = "Test123!"
CHALLENGE = bytes(range(16))


def receive(sock, length):
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise EOFError("Viewer closed the connection")
        data.extend(chunk)
    return bytes(data)


class UnifiedPanelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="unified-vnc-")
        cls.directory = Path(cls.temp.name)
        cls.cert = cls.directory / "panel.pem"
        cls.key = cls.directory / "panel-key.pem"
        subprocess.run([
            OPENSSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(cls.key), "-out", str(cls.cert), "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
        ], check=True, capture_output=True)
        # VNC DES reverses the bits of each password byte.
        key = bytes(int(f"{byte:08b}"[::-1], 2) for byte in PASSWORD.encode())
        cls.response = subprocess.run([
            OPENSSL, "enc", "-des-ecb", "-provider", "legacy",
            "-provider", "default", "-K", key.hex(), "-nopad",
        ], input=CHALLENGE, check=True, capture_output=True).stdout

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @contextlib.contextmanager
    def viewer(self, *options, config=None, cancel_after=False):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(15)
            port = listener.getsockname()[1]
            command = [VIEWER]
            if config:
                config_path = self.directory / "panel.tigervnc"
                config_path.write_text(
                    "TigerVNC Configuration file Version 1.0\n" + config +
                    f"ServerName=127.0.0.1::{port}\n",
                    encoding="utf-8")
                command.append(str(config_path))
            else:
                command.extend(["-UnifiedPanel=1", f"127.0.0.1::{port}"])
            command.extend([
                "-ViewOnly=1", "-AlertOnFatalError=0",
                "-ReconnectOnError=0", "-X509CA", str(self.cert),
                *options,
            ])
            # Isolate Linux viewer settings and certificate trust from the user.
            env = {**os.environ, "VNC_PASSWORD": PASSWORD,
                   "XDG_CONFIG_HOME": str(self.directory / "config"),
                   "XDG_STATE_HOME": str(self.directory / "state")}
            startup = None
            if os.name == "nt":
                startup = subprocess.STARTUPINFO()
                startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = subprocess.SW_HIDE
            with tempfile.TemporaryFile() as log:
                process = subprocess.Popen(command, env=env, stdout=log, stderr=log,
                                           startupinfo=startup)
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(10)
                        yield connection
                    if not cancel_after:
                        process.wait(timeout=10)
                except Exception:
                    log.seek(0)
                    print(log.read().decode(errors="replace"))
                    raise
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()

    def rfb(self, connection, types=b"\x13"):
        # Send the banner in fragments to exercise stream buffering.
        connection.sendall(b"RFB 003.")
        connection.sendall(b"008\n")
        self.assertEqual(receive(connection, 12), b"RFB 003.008\n")
        connection.sendall(bytes([len(types)]) + types)

    def vencrypt(self, connection, subtypes):
        self.rfb(connection)
        self.assertEqual(receive(connection, 1), b"\x13")
        connection.sendall(b"\x00\x02")
        self.assertEqual(receive(connection, 2), b"\x00\x02")
        connection.sendall(b"\x00" + bytes([len(subtypes)]) +
                           b"".join(struct.pack("!I", item) for item in subtypes))

    def expect_closed(self, connection):
        try:
            self.assertEqual(connection.recv(1), b"", "Unexpected insecure selection")
        except ConnectionResetError:
            pass

    def session(self, connection, anonymous=False):
        chosen = 258 if anonymous else 261
        self.vencrypt(connection, [1, 2, 257, 260, chosen])
        self.assertEqual(struct.unpack("!I", receive(connection, 4))[0], chosen)
        connection.sendall(b"\x01")
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        if anonymous:
            tls.maximum_version = ssl.TLSVersion.TLSv1_2
            tls.set_ciphers("AECDH-AES256-SHA:@SECLEVEL=0")
        else:
            tls.load_cert_chain(self.cert, self.key)
        with tls.wrap_socket(connection, server_side=True) as secure:
            secure.sendall(CHALLENGE)
            self.assertEqual(receive(secure, 16), self.response)
            secure.sendall(struct.pack("!I", 0))
            self.assertEqual(receive(secure, 1), b"\x01", "Session must be shared")
            # ServerInit: 64 x 48, little-endian BGRX, followed by the desktop name.
            name = b"Unified panel fixture"
            pixel_format = struct.pack("!BBBBHHHBBB3x", 32, 24, 0, 1,
                                       255, 255, 255, 16, 8, 0)
            secure.sendall(struct.pack("!HH", 64, 48) + pixel_format +
                           struct.pack("!I", len(name)) + name)
            requested = False
            for _ in range(20):
                message = receive(secure, 1)[0]
                if message == 0:  # SetPixelFormat
                    receive(secure, 19)
                elif message == 2:  # SetEncodings
                    count = struct.unpack("!H", receive(secure, 3)[1:])[0]
                    receive(secure, count * 4)
                elif message == 3:  # FramebufferUpdateRequest
                    receive(secure, 9)
                    requested = True
                    break
                else:
                    self.fail(f"Unexpected client message before framebuffer request: {message}")
            self.assertTrue(requested, "Viewer never requested a framebuffer")
            # Send a raw rectangle, then verify the viewer requests another frame.
            secure.sendall(struct.pack("!BBHHHHHi", 0, 0, 1, 0, 0, 64, 48, 0) +
                           b"\x20\x80\xe0\x00" * (64 * 48))
            for _ in range(20):
                message = receive(secure, 1)[0]
                if message == 3:
                    receive(secure, 9)
                    break
                if message == 0:
                    receive(secure, 19)
                elif message == 2:
                    count = struct.unpack("!H", receive(secure, 3)[1:])[0]
                    receive(secure, count * 4)
                else:
                    self.fail(f"Unexpected client message after framebuffer: {message}")
            else:
                self.fail("Viewer did not consume the framebuffer")

    def test_certificate_tls_password_and_framebuffer(self):
        with self.viewer() as connection:
            self.session(connection)

    def test_explicit_anonymous_tls_password_and_framebuffer(self):
        with self.viewer("-UnifiedSecurity=AnonymousTLS") as connection:
            self.session(connection, anonymous=True)

    def test_command_line_cannot_override_profile_security(self):
        with self.viewer("-SecurityTypes=None,VncAuth", "-Shared=0",
                         "-RemoteResize=1", "-DesktopSize=1920x1080") as connection:
            self.session(connection)

    def test_saved_profile_security_overrides_insecure_settings(self):
        with self.viewer(config="UnifiedPanel=1\nUnifiedSecurity=Certificate\n"
                         "SecurityTypes=None,VncAuth\nShared=0\n") as connection:
            self.session(connection)

    def test_saved_anonymous_mode_is_restored(self):
        with self.viewer(config="UnifiedPanel=1\nUnifiedSecurity=AnonymousTLS\n") as connection:
            self.session(connection, anonymous=True)

    def test_untrusted_certificate_needs_a_decision(self):
        with self.viewer("-X509CA", str(self.directory / "missing-ca.pem"),
                         cancel_after=True) as connection:
            self.vencrypt(connection, [261])
            self.assertEqual(receive(connection, 4), struct.pack("!I", 261))
            connection.sendall(b"\x01")
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            # TLS 1.3's final client flight can remain buffered while the native
            # Windows viewer displays the trust prompt. TLS 1.2 lets this fixture
            # finish its handshake before asserting no password response arrives.
            tls.minimum_version = ssl.TLSVersion.TLSv1_2
            tls.maximum_version = ssl.TLSVersion.TLSv1_2
            tls.load_cert_chain(self.cert, self.key)
            with tls.wrap_socket(connection, server_side=True) as secure:
                secure.sendall(CHALLENGE)
                secure.settimeout(1)
                with self.assertRaises(TimeoutError):
                    receive(secure, 16)

    def test_plaintext_vnc_is_rejected(self):
        with self.viewer() as connection:
            self.rfb(connection, b"\x01\x02")
            self.expect_closed(connection)

    def test_passwordless_tls_is_rejected(self):
        with self.viewer() as connection:
            self.vencrypt(connection, [257, 260])
            self.expect_closed(connection)

    def test_certificate_mode_rejects_anonymous_tls(self):
        with self.viewer() as connection:
            self.vencrypt(connection, [258])
            self.expect_closed(connection)

    def test_standard_vnc_still_available(self):
        with self.viewer("-UnifiedPanel=0", "-SecurityTypes=VncAuth") as connection:
            self.rfb(connection, b"\x02")
            self.assertEqual(receive(connection, 1), b"\x02")
            connection.sendall(CHALLENGE)
            self.assertEqual(receive(connection, 16), self.response)
            reason = b"End of standard VNC test"
            connection.sendall(struct.pack("!II", 1, len(reason)) + reason)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", required=True, help="Path to the built native viewer")
    parser.add_argument("--openssl", default="openssl", help="OpenSSL executable (default: PATH)")
    args, remaining = parser.parse_known_args()
    VIEWER = str(Path(args.viewer).resolve())
    OPENSSL = args.openssl
    unittest.main(argv=[__file__, *remaining], verbosity=2)
