#!/usr/bin/env python3
"""Multi-panel dashboard tests. Every server binds only to 127.0.0.1.

Native input tests require an isolated Xvfb display. Stored credentials belong
only to temporary fixtures; Linux keyring tests require an isolated D-Bus session.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
import argparse
import contextlib
import ctypes
import os
from pathlib import Path
import socket
import sqlite3
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import uuid
import unified_panel as protocol

VIEWER = None
INPUT_TESTS = False
SCREENSHOT = None


def wait_for(predicate, message, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.04)
    raise AssertionError(message)


def protected_password(identifier, password):
    if os.name == "nt":
        class Blob(ctypes.Structure):
            _fields_ = [("size", ctypes.c_ulong), ("data", ctypes.c_void_p)]
        raw = ctypes.create_string_buffer(password.encode())
        purpose = ("SuperSmartClient/dashboard/" + identifier).encode()
        entropy = ctypes.create_string_buffer(purpose)
        source = Blob(len(password.encode()), ctypes.addressof(raw))
        extra = Blob(len(purpose), ctypes.addressof(entropy))
        output = Blob()
        if not ctypes.windll.crypt32.CryptProtectData(ctypes.byref(source), "Fixture", ctypes.byref(extra),
                                                     None, None, 1, ctypes.byref(output)):
            raise ctypes.WinError()
        try:
            return "dpapi:" + ctypes.string_at(output.data, output.size).hex()
        finally:
            ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.LocalFree(output.data)
    if os.environ.get("SUPERSMART_TEST_KEYRING") != "1":
        raise unittest.SkipTest("Run with tests/integration/isolated-keyring.sh on Linux")
    subprocess.run(["secret-tool", "store", "--label=SuperSmartClient test", "xdg:schema",
                    "org.supersmartclient.Panel", "id", identifier],
                   input=password.encode(), check=True, capture_output=True, timeout=10)
    return "keyring:" + identifier


def panel_profile(server, name, *, monitor=False, auto=True, saved=True):
    identifier = uuid.uuid4().hex
    return dict(id=identifier, name=name, address=f"127.0.0.1::{server.port}",
                credential=protected_password(identifier, server.password) if saved else "",
                security="Standard" if server.standard else "Certificate", remember=1,
                reconnect=1, autoConnect=int(auto), viewOnly=int(monitor), columns=1, rows=1)


def workspace(profiles):
    output = "SuperSmartClient Dashboard 1\ncolumns=2\nrowHeight=260\nwidth=1320\nheight=860\ndark=0\n"
    for panel in profiles:
        output += f"\n[panel {panel['id']}]\n"
        for key, value in panel.items():
            if key == "id":
                continue
            if key in ("name", "address"):
                value = value.encode().hex()
            output += f"{key}={value}\n"
    return output


def create_database(path, layouts):
    with contextlib.closing(sqlite3.connect(path)) as db:
        db.executescript("PRAGMA user_version=1; PRAGMA application_id=1397965636;"
                         "CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                         "CREATE TABLE layouts(id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE, "
                         "workspace TEXT NOT NULL, position INTEGER NOT NULL);")
        for number, (name, profiles) in enumerate(layouts):
            db.execute("INSERT INTO layouts VALUES (?,?,?,?)", (str(number), name, workspace(profiles), number))
        db.execute("INSERT INTO metadata VALUES ('active','0')")
        db.commit()


def database_layout(path, number=0):
    with contextlib.closing(sqlite3.connect(path)) as db:
        return db.execute("SELECT workspace FROM layouts WHERE id=?", (str(number),)).fetchone()[0]


def saved_profiles(path):
    return [dict(line.split("=", 1) for line in section.splitlines()[1:] if "=" in line)
            for section in database_layout(path).split("[panel ")[1:]]


class Server:
    def __init__(self, fixture, password="Test123!", standard=False, hold=False, reject=False, size=(320, 180), detail=False):
        self.fixture, self.password, self.standard, self.hold, self.reject = fixture, password, standard, hold, reject
        self.connections, self.frames, self.authentications = 0, 0, 0
        self.inputs, self.errors = [], []
        self.size, self.detail = size, detail
        self.stop, self.drop = threading.Event(), threading.Event()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(5)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        key = bytes(int(f"{byte:08b}"[::-1], 2) for byte in password.encode()[:8].ljust(8, b"\0"))
        self.response = subprocess.run([protocol.OPENSSL, "enc", "-des-ecb", "-provider", "legacy",
            "-provider", "default", "-K", key.hex(), "-nopad"],
            input=protocol.CHALLENGE, check=True, capture_output=True).stdout
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=6)
        self.listener.close()

    def run(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            self.connections += 1
            self.active_socket = connection
            try:
                with connection:
                    connection.settimeout(5)
                    if self.hold:
                        while not self.stop.wait(0.1):
                            pass
                        continue
                    self.session(connection)
            except (EOFError, ConnectionError, ssl.SSLEOFError):
                pass
            except Exception as error:
                if not self.stop.is_set():
                    self.errors.append(repr(error))
            finally:
                self.active_socket.close()

    def session(self, connection):
        receive = protocol.receive
        if self.standard:
            connection.sendall(b"RFB 003.008\n")
            assert receive(connection, 12) == b"RFB 003.008\n"
            connection.sendall(b"\x01\x01")
            assert receive(connection, 1) == b"\x01"
            secure = connection
        else:
            connection.sendall(b"VNC OVER SSLRFB 003.008\n")
            secure = self.fixture.certificate_context().wrap_socket(connection, server_side=True)
            self.active_socket = secure
            assert receive(secure, 12) == b"RFB 003.008\n"
            secure.sendall(b"\x02\x10\x02")
            assert receive(secure, 1) == b"\x02"
            secure.sendall(protocol.CHALLENGE)
            response = receive(secure, 16)
            self.authentications += 1
            if self.reject:
                reason = b"Password rejected"
                secure.sendall(struct.pack("!II", 1, len(reason)) + reason)
                secure.close()
                return
            assert response == self.response, "Wrong stored password reached this panel"
        with secure:
            secure.sendall(struct.pack("!I", 0))
            assert receive(secure, 1) == b"\x01", "Connection must be shared"
            pixel_format = struct.pack("!BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 0, 8, 16)
            name = b"Loopback dashboard fixture"
            width, height = self.size
            secure.sendall(struct.pack("!HH", width, height) + pixel_format + struct.pack("!I", len(name)) + name)
            secure.settimeout(0.2)
            # Simple instrument-like test picture, generated without external assets.
            pixels = bytearray()
            for y in range(height):
                for x in range(width):
                    if y < 28:
                        rgb = (35, 72, 111)
                    elif 20 < x < 300 and 50 < y < 140:
                        rgb = (45, 161, 122) if x < 190 else (227, 235, 244)
                    else:
                        rgb = (244, 247, 250)
                    if self.detail:
                        rgb = (244, 247, 250) if y >= 28 else (35, 72, 111)
                        if 32 <= x < 288 and 64 <= y < 192:
                            value = 255 if (x + y) % 2 else 0
                            rgb = (value, value, value)
                        elif 320 <= x < 608 and 64 <= y < 192:
                            value = 0 if x % 12 == 1 else 255
                            rgb = (value, value, value)
                    pixels.extend((*rgb, 0))
            if self.detail:
                # Small bitmap numerals exercise thin text at several sizes.
                glyphs = [
                    ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
                    ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
                    ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
                    ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
                    ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
                    ["11111", "10000", "10000", "11110", "00001", "00001", "11110"],
                    ["01110", "10000", "10000", "11110", "10001", "10001", "01110"],
                    ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
                    ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
                    ["01110", "10001", "10001", "01111", "00001", "00001", "01110"],
                ]
                for row, scale in enumerate((1, 2, 3, 4)):
                    for digit, glyph in enumerate(glyphs):
                        for gy, bits in enumerate(glyph):
                            for gx, bit in enumerate(bits):
                                if bit != "1": continue
                                for dy in range(scale):
                                    for dx in range(scale):
                                        x, y = 40 + digit * 8 * scale + gx * scale + dx, 240 + row * 90 + gy * scale + dy
                                        if x < width and y < height:
                                            offset = (y * width + x) * 4
                                            pixels[offset:offset + 4] = bytes((24, 39, 60, 0))
            while not self.stop.is_set():
                if self.drop.is_set():
                    self.drop.clear()
                    return
                try:
                    message = receive(secure, 1)[0]
                except socket.timeout:
                    continue
                if message == 0:
                    receive(secure, 19)
                elif message == 2:
                    count = struct.unpack("!H", receive(secure, 3)[1:])[0]
                    receive(secure, count * 4)
                elif message == 3:
                    receive(secure, 9)
                    secure.sendall(struct.pack("!BBHHHHHi", 0, 0, 1, 0, 0, width, height, 0) + pixels)
                    self.frames += 1
                elif message == 4:
                    self.inputs.append(("key", *struct.unpack("!B2xI", receive(secure, 7))))
                elif message == 5:
                    self.inputs.append(("pointer", *struct.unpack("!BHH", receive(secure, 5))))
                else:
                    raise AssertionError(f"Unexpected client message: {message}")


class DashboardTests(unittest.TestCase):
    certificate_context = protocol.UnifiedPanelTests.certificate_context

    @classmethod
    def setUpClass(cls):
        protocol.UnifiedPanelTests.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        protocol.UnifiedPanelTests.tearDownClass.__func__(cls)

    def setUp(self):
        self.servers = []
        self.case_dir = self.directory / uuid.uuid4().hex
        self.case_dir.mkdir()
        self.db = self.case_dir / "dashboard.db"

    def tearDown(self):
        for server in self.servers:
            server.close()
        for server in self.servers:
            self.assertEqual(server.errors, [])

    def server(self, **kwargs):
        server = Server(self, **kwargs)
        self.servers.append(server)
        return server

    @contextlib.contextmanager
    def viewer(self):
        env = {**os.environ, "XDG_CONFIG_HOME": str(self.case_dir / "config"),
               "XDG_STATE_HOME": str(self.case_dir / "state")}
        env.pop("VNC_PASSWORD", None)
        startup = None
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen([VIEWER, "-DashboardConfig", str(self.db), "-X509CA", str(self.cert)],
                                       env=env, stdout=log, stderr=log, startupinfo=startup)
            self.process = process
            try:
                yield process
                self.assertIsNone(process.poll(), "Dashboard unexpectedly exited")
            except Exception:
                log.seek(0)
                print(log.read().decode(errors="replace"))
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    def test_independent_reconnect_and_stored_passwords_after_restart(self):
        first, second = self.server(password="First123"), self.server(password="Second12")
        create_database(self.db, [("Production", [panel_profile(first, "Press line"), panel_profile(second, "Packing line")])])
        with self.viewer():
            wait_for(lambda: first.frames > 2 and second.frames > 2, "Both panels did not stream")
            previous = second.frames
            first.drop.set()
            wait_for(lambda: first.authentications == 2 and first.frames > 5, "Dropped panel did not reconnect")
            self.assertEqual(second.connections, 1)
            self.assertGreater(second.frames, previous)
            self.assertEqual(first.inputs + second.inputs, [])
        with self.viewer():
            wait_for(lambda: first.authentications == 3 and second.authentications == 2,
                     "Saved passwords were not restored after app restart")

    def test_password_rejection_pauses_reconnect(self):
        server = self.server(reject=True)
        create_database(self.db, [("Auth failure", [panel_profile(server, "Rejected password")])])
        with self.viewer():
            wait_for(lambda: server.authentications == 1, "Authentication never attempted")
            time.sleep(3)
            self.assertEqual(server.connections, 1, "Rejected password was retried automatically")

    def test_missing_password_pauses_without_authentication(self):
        server = self.server()
        create_database(self.db, [("Missing password", [panel_profile(server, "Enter password", saved=False)])])
        with self.viewer():
            wait_for(lambda: server.connections == 1, "Panel never connected")
            time.sleep(3)
            self.assertEqual(server.connections, 1)
            self.assertEqual(server.authentications, 0)

    def test_silent_server_does_not_block_other_panels(self):
        silent, live = self.server(hold=True), self.server(standard=True)
        create_database(self.db, [("Independent", [panel_profile(silent, "Silent", saved=False),
                                                   panel_profile(live, "Standard VNC", saved=False)])])
        with self.viewer():
            wait_for(lambda: live.frames > 2, "Silent server blocked the second session")
            self.assertEqual(silent.connections, 1)

    def xdo(self, *args):
        return subprocess.run(["xdotool", *map(str, args)], capture_output=True, text=True,
                              check=True, timeout=5).stdout.strip()

    def require_input(self):
        if not INPUT_TESTS:
            self.skipTest("Use --input-tests on an isolated Xvfb display")
        display = os.environ.get("DISPLAY", "")
        self.assertTrue(display.startswith(":"))
        number = display[1:].split(".")[0]
        self.assertTrue(number.isdigit())
        pid = Path(f"/tmp/.X{number}-lock").read_text().strip()
        self.assertEqual(Path(f"/proc/{pid}/comm").read_text().strip(), "Xvfb")

    def window(self):
        # FLTK recreates an untitled override-redirect window for fullscreen
        # without a window manager. Keep targeting this fixture's PID and choose
        # its main window by area, excluding any small tooltip windows.
        windows = self.xdo("search", "--all", "--sync", "--onlyvisible", "--pid", self.process.pid).splitlines()
        window = max(windows, key=lambda item: self.geometry(item)["WIDTH"] * self.geometry(item)["HEIGHT"])
        self.assertEqual(int(self.xdo("getwindowpid", window)), self.process.pid)
        self.xdo("windowfocus", "--sync", window)
        return window

    def click(self, window, x, y):
        self.xdo("mousemove", "--window", window, x, y)
        self.xdo("click", 1)
        time.sleep(0.08)

    def drag(self, window, x1, y1, x2, y2):
        self.xdo("mousemove", "--window", window, x1, y1)
        # XTest button state is needed for motion to be delivered as a drag.
        # require_input() has verified this is an isolated Xvfb display.
        self.xdo("mousedown", 1)
        time.sleep(0.06)
        self.xdo("mousemove", "--window", window, x2, y2)
        # Allow FLTK to dispatch its consolidated motion before button release.
        time.sleep(0.08)
        self.xdo("mouseup", 1)
        time.sleep(0.2)

    def choose_preset(self, window, value):
        settings = dict(line.split("=", 1) for line in database_layout(self.db).split("[panel ")[0].splitlines() if "=" in line)
        current = int(settings.get("preset", 0))
        self.click(window, 310, 20)
        steps = ["Down" if value > current else "Up"] * abs(value - current)
        self.xdo("key", *steps, "Return")
        wait_for(lambda: f"preset={value}\n" in database_layout(self.db), "Arrangement selection was not saved")

    def picture(self, window, suffix):
        path = str(SCREENSHOT) + suffix if SCREENSHOT else str(self.case_dir / (suffix + ".png"))
        time.sleep(0.15)
        return capture_window(window, path)

    @staticmethod
    def pixel(picture, x, y):
        _, _, raw, stride = picture
        offset = y * stride + x * 4
        return tuple(raw[offset + channel] for channel in (2, 1, 0))

    def geometry(self, window):
        return {key: int(value) for key, value in (line.split("=", 1) for line in
                self.xdo("getwindowgeometry", "--shell", window).splitlines()) if key in ("X", "Y", "WIDTH", "HEIGHT")}

    def test_grid_order_size_focus_and_layout_dropdown(self):
        self.require_input()
        first, second, third = self.server(), self.server(), self.server()
        profiles = [panel_profile(first, "Press line - North"), panel_profile(second, "Packing line - South", monitor=True)]
        create_database(self.db, [("Production floor", profiles),
                                 ("Maintenance", [panel_profile(third, "Maintenance bench", auto=False)])])
        with self.viewer():
            wait_for(lambda: first.frames > 2 and second.frames > 2, "Panels did not stream")
            window = self.window()
            if SCREENSHOT:
                capture_window(window, str(SCREENSHOT) + ".grid.png")
            self.drag(window, 70, 56, 720, 56)
            encoded = database_layout(self.db)
            self.assertLess(encoded.index(profiles[1]["id"]), encoded.index(profiles[0]["id"]))
            self.drag(window, 660, 450, 790, 450)
            encoded = database_layout(self.db)
            self.assertIn("columnWeights=784,524", encoded)
            self.assertEqual([p["columns"] for p in saved_profiles(self.db)], ["1", "1"])
            self.assertEqual(first.inputs + second.inputs, [], "Grid gestures reached a panel")
            self.click(window, 744, 56)  # Focus the first pane.
            focused = self.picture(window, ".focused.png")
            self.assertEqual(self.pixel(focused, 1315, 500), (37, 99, 235), "Focused pane does not fill the workspace")
            self.click(window, 640, 20)  # Overview
            # Select the next named layout. The saved startup flag is false;
            # opening the layout must nevertheless connect the whole group.
            self.click(window, 130, 20)
            self.xdo("key", "Down", "Return")
            if SCREENSHOT:
                time.sleep(0.2)
                capture_window(window, str(SCREENSHOT) + ".dropdown.png")
            wait_for(lambda: third.frames > 2, "Dropdown did not open and connect the saved layout")
            with contextlib.closing(sqlite3.connect(self.db)) as db:
                self.assertEqual(db.execute("SELECT value FROM metadata WHERE key='active'").fetchone()[0], "1")
            self.assertEqual(first.inputs + second.inputs + third.inputs, [])

    def test_control_and_monitor_tiles_keep_input_separate(self):
        self.require_input()
        control, monitor = self.server(), self.server()
        create_database(self.db, [("Input fixture", [panel_profile(control, "Control"),
                                                      panel_profile(monitor, "Monitor", monitor=True)])])
        with self.viewer():
            wait_for(lambda: control.frames > 2 and monitor.frames > 2, "Panels did not stream")
            window = self.window()
            self.click(window, 333, 450)
            self.xdo("key", "--window", window, "a")
            wait_for(lambda: ("key", 1, ord("a")) in control.inputs and ("key", 0, ord("a")) in control.inputs,
                     "Control tile did not forward key press/release")
            self.assertTrue(any(event[0] == "pointer" and event[1] & 1 for event in control.inputs))
            self.click(window, 988, 450)
            self.xdo("key", "--window", window, "b")
            time.sleep(0.3)
            self.assertEqual(monitor.inputs, [], "Monitor tile forwarded input")

    def test_add_panel_form_saves_password_and_reopens(self):
        self.require_input()
        server = self.server()
        create_database(self.db, [("New workspace", [])])
        with self.viewer():
            window = self.window()
            self.click(window, 465, 20)
            editor = self.xdo("search", "--all", "--sync", "--onlyvisible", "--pid", self.process.pid,
                              "--name", "Add panel - SuperSmartClient").splitlines()[0]
            self.assertEqual(int(self.xdo("getwindowpid", editor)), self.process.pid)
            self.xdo("windowfocus", "--sync", editor)
            for x, y, value in [(70, 103, "New panel fixture"), (70, 168, f"127.0.0.1::{server.port}"),
                                 (70, 325, server.password)]:
                self.click(editor, x, y)
                self.xdo("type", "--delay", 1, value)
            if SCREENSHOT:
                time.sleep(0.25)
                capture_window(editor, str(SCREENSHOT) + ".editor.png")
            self.click(editor, 430, 500)
            wait_for(lambda: server.frames > 2, "Saved panel did not connect")
            encoded = database_layout(self.db)
            self.assertNotIn(server.password, encoded)
            self.assertNotIn(server.password.encode().hex(), encoded)
            self.assertIn("keyring:", encoded)
        with self.viewer():
            wait_for(lambda: server.authentications == 2, "Newly saved password was not restored")

    def test_fit_window_with_four_panels(self):
        self.require_input()
        servers = [self.server() for _ in range(4)]
        names = ["Press line - North", "Packing line - South", "Utilities", "Assembly"]
        profiles = [panel_profile(server, name, monitor=number > 1)
                    for number, (server, name) in enumerate(zip(servers, names))]
        create_database(self.db, [("Production floor", profiles)])
        with contextlib.closing(sqlite3.connect(self.db)) as db:
            db.execute("UPDATE layouts SET workspace=replace(workspace, 'rowHeight=260', 'rowHeight=0')")
            db.commit()
        with self.viewer():
            wait_for(lambda: all(server.frames > 3 for server in servers), "Four panels did not stream")
            window = self.window()
            if SCREENSHOT:
                time.sleep(0.2)
                capture_window(window, SCREENSHOT)
            previous = [server.frames for server in servers]
            self.xdo("windowsize", window, 1160, 780)
            wait_for(lambda: all(server.frames > before + 2 for server, before in zip(servers, previous)),
                     "Panels stopped updating when the dashboard was resized")
            self.assertEqual([event for server in servers for event in server.inputs], [])

    def test_layout_menu_creates_named_copy(self):
        self.require_input()
        create_database(self.db, [("Original", [])])
        with self.viewer():
            window = self.window()
            self.click(window, 553, 20)
            # Open the second menu entry, Save layout as.
            self.xdo("key", "Down", "Return")
            self.xdo("type", "--delay", 1, "Saved copy")
            self.xdo("key", "Return")
            def copied():
                with contextlib.closing(sqlite3.connect(self.db)) as db:
                    return db.execute("SELECT count(*) FROM layouts WHERE name='Saved copy'").fetchone()[0] == 1
            wait_for(copied, "Layout menu did not save a named copy")

    def test_moved_window_menu_anchor_round_buttons_and_dark_fields(self):
        self.require_input()
        create_database(self.db, [("Appearance fixture", [])])
        with self.viewer():
            window = self.window()
            self.xdo("windowmove", window, 120, 100)
            self.click(window, 1298, 20)  # Workspace actions on the isolated empty dashboard.
            self.xdo("key", "Down", "Down", "Down", "Down", "Return")  # Dark theme.
            def picture(target, suffix):
                path = str(SCREENSHOT) + suffix if SCREENSHOT else str(self.case_dir / (suffix + ".png"))
                return capture_window(target, path)
            def pixel(picture, x, y):
                width, height, raw, stride = picture
                self.assertTrue(0 <= x < width and 0 <= y < height)
                offset = y * stride + x * 4
                return tuple(raw[offset + channel] for channel in (2, 1, 0))
            time.sleep(0.15)
            dark = picture(window, ".dark.png")
            self.assertEqual(pixel(dark, 200, 10), (30, 41, 59), "Layout field is not dark")
            self.assertEqual(pixel(dark, 260, 10), (30, 41, 59), "Arrangement field is not dark")
            self.assertEqual(pixel(dark, 418, 5), (30, 41, 59), "Button corner is square")
            self.assertEqual(pixel(dark, 460, 7), (37, 99, 235), "Primary button fill is wrong")
            visible = root_windows()
            self.click(window, 553, 20)
            popups = root_windows() - visible
            def geometry(target):
                return dict(line.split("=", 1) for line in self.xdo("getwindowgeometry", "--shell", target).splitlines())
            menu = next(target for target in popups if int(geometry(target)["HEIGHT"]) > 100)
            parent_bounds, menu_bounds = geometry(window), geometry(menu)
            self.assertAlmostEqual(int(menu_bounds["X"]), int(parent_bounds["X"]) + 518, delta=3)
            self.assertAlmostEqual(int(menu_bounds["Y"]), int(parent_bounds["Y"]) + 35, delta=3)
            popup = picture(menu, ".menu.png")
            self.assertLess(max(pixel(popup, 3, 3)), 100, "Dark popup has a light background")
            self.xdo("key", "Escape")
            self.click(window, 465, 20)
            editor = self.xdo("search", "--all", "--sync", "--onlyvisible", "--pid", self.process.pid,
                              "--name", "Add panel - SuperSmartClient").splitlines()[0]
            time.sleep(0.1)
            form = picture(editor, ".dark-editor.png")
            self.assertEqual(pixel(form, 10, 10), (17, 24, 39), "Dialog did not inherit dark mode")
            self.assertEqual(pixel(form, 40, 100), (30, 41, 59), "Text input has a light background")
            self.assertEqual(pixel(form, 100, 244), (30, 41, 59), "Dialog dropdown has a light background")
            self.click(editor, 75, 500)  # Cancel.

    def assert_checkbox(self, picture, left, top, height, checked):
        # The indicator has an 18-pixel square and a contrasting white tick.
        x, y = left + 2, top + (height - 18) // 2
        pixels = [self.pixel(picture, x + dx, y + dy) for dx in range(2, 16) for dy in range(2, 16)]
        blue = sum(pixel == (37, 99, 235) for pixel in pixels)
        white = sum(min(pixel) > 230 for pixel in pixels)
        if checked:
            self.assertGreater(blue, 95, "Checked box has no visible filled indicator")
            self.assertGreater(white, 12, "Checked box has no visible white tick")
        else:
            self.assertEqual(blue, 0, "Unchecked box still appears checked")

    def test_checkbox_states_are_visible_and_toggle_with_mouse_and_keyboard(self):
        self.require_input()
        create_database(self.db, [("Checkbox fixture", [])])
        with self.viewer():
            window = self.window()
            for theme in ("light", "dark"):
                if theme == "dark":
                    self.click(window, 1298, 20)
                    self.xdo("key", "Down", "Down", "Down", "Down", "Return")
                self.click(window, 465, 20)
                editor = self.xdo("search", "--all", "--sync", "--onlyvisible", "--pid", self.process.pid,
                                  "--name", "Add panel - SuperSmartClient").splitlines()[0]
                self.xdo("windowfocus", "--sync", editor)
                before = self.picture(editor, f".checkbox-{theme}.png")
                self.assert_checkbox(before, 28, 352, 25, True)
                self.assert_checkbox(before, 28, 383, 25, True)
                self.assert_checkbox(before, 282, 383, 25, True)
                self.assert_checkbox(before, 28, 414, 25, False)
                self.click(editor, 130, 426)  # Clicking the label checks Monitor.
                self.click(editor, 150, 364)  # Uncheck Remember.
                changed = self.picture(editor, f".checkbox-{theme}-changed.png")
                self.assert_checkbox(changed, 28, 414, 25, True)
                self.assert_checkbox(changed, 28, 352, 25, False)
                self.xdo("key", "space")  # Focused checkbox toggles with Space.
                restored = self.picture(editor, f".checkbox-{theme}-keyboard.png")
                self.assert_checkbox(restored, 28, 352, 25, True)
                self.click(editor, 75, 500)

    def test_filtered_scaling_preserves_fine_detail_and_native_pixels(self):
        self.require_input()
        server = self.server(size=(1280, 800), detail=True)
        profile = panel_profile(server, "Resolution chart")
        profile.update(displayWidth=1280, displayHeight=800, displayPreset=3, scale=50, fit=0,
                       freePositioned=1, pixelX=0, pixelY=0, pixelWidth=642, pixelHeight=426)
        create_database(self.db, [("Image quality", [profile])])
        with self.viewer():
            wait_for(lambda: server.frames > 2, "Resolution chart did not stream")
            window = self.window()
            self.choose_preset(window, 9)
            current = 50
            for percent in (50, 25, 100):
                if percent != current:
                    self.click(window, 4 + 1280 * current // 100 + 2 - 16, 56)
                    self.xdo("key", *(7 * ["Down"]), "Return")
                    dialog = self.xdo("search", "--all", "--sync", "--onlyvisible", "--pid", self.process.pid,
                                      "--name", "Panel size and scale").splitlines()[0]
                    self.xdo("windowfocus", "--sync", dialog)
                    self.click(dialog, 65, 140)
                    self.xdo("key", "ctrl+a")
                    self.xdo("type", str(percent))
                    self.click(dialog, 420, 288)
                    current = percent
                picture = self.picture(window, f".quality-{percent}.png")
                if percent < 100:
                    for y in range(96 * percent // 100, 160 * percent // 100):
                        for x in range(96 * percent // 100, 224 * percent // 100):
                            for channel in self.pixel(picture, 5 + x, 69 + y):
                                self.assertAlmostEqual(channel, 127.5, delta=2,
                                                       msg="Shrinking loses detail or produces aliasing")
                else:
                    for y in range(96, 112):
                        for x in range(96, 112):
                            expected = 255 if (x + y) % 2 else 0
                            self.assertEqual(self.pixel(picture, 5 + x, 69 + y), (expected,) * 3,
                                             "Native resolution was filtered or reused a scaled image")
            self.assertEqual(server.inputs, [], "Local scaling controls reached the panel")

    def test_presets_reorder_into_fixed_slots_without_remote_input(self):
        self.require_input()
        servers = [self.server() for _ in range(3)]
        profiles = [panel_profile(server, name) for server, name in zip(servers, ["North", "South", "Utilities"])]
        create_database(self.db, [("Three panels", profiles)])
        with self.viewer():
            wait_for(lambda: all(server.frames > 2 for server in servers), "Panels did not stream")
            window = self.window()
            self.click(window, 310, 20)
            self.xdo("key", "Down", "Down", "Down", "Down", "Return")
            wait_for(lambda: "preset=4\n" in database_layout(self.db), "Three-panel preset was not saved")
            saved = saved_profiles(self.db)
            self.assertEqual([p["columns"] for p in saved], ["2", "1", "1"])
            if SCREENSHOT:
                time.sleep(0.2)
                capture_window(window, str(SCREENSHOT) + ".three.png")
            # Reordering keeps the preset shape, moving a different panel into the top slot.
            self.drag(window, 70, 56, 80, 464)
            saved = saved_profiles(self.db)
            self.assertEqual(saved[0]["name"], profiles[1]["name"].encode().hex())
            self.assertEqual([p["columns"] for p in saved], ["2", "1", "1"])
            self.click(window, 70, 56)
            self.xdo("key", "a")  # Header selection must not focus the HMI keyboard.
            time.sleep(0.15)
            self.assertEqual([event for server in servers for event in server.inputs], [])

    def test_individual_scaling_free_move_resize_and_restore(self):
        self.require_input()
        first, second = self.server(size=(320, 200)), self.server()
        profiles = [panel_profile(first, "12 inch - North"), panel_profile(second, "15 inch - South")]
        create_database(self.db, [("Scaled panels", profiles)])
        with self.viewer():
            wait_for(lambda: first.frames > 2 and second.frames > 2, "Panels did not stream")
            window = self.window()
            self.click(window, 643, 56)  # First tile's local menu.
            self.xdo("key", *(7 * ["Down"]), "Return")
            dialog = self.xdo("search", "--all", "--sync", "--onlyvisible", "--pid", self.process.pid,
                               "--name", "Panel size and scale").splitlines()[0]
            self.xdo("windowfocus", "--sync", dialog)
            self.click(dialog, 160, 72)
            self.xdo("key", "Down", "Down", "Down", "Return")  # MTP1200 1280 x 800
            self.click(dialog, 164, 140)  # Disable Fit.
            self.click(dialog, 65, 140)
            self.xdo("key", "ctrl+a")
            self.xdo("type", "50")
            if SCREENSHOT:
                capture_window(dialog, str(SCREENSHOT) + ".scale-dialog.png")
            self.click(dialog, 420, 288)
            wait_for(lambda: "preset=9\n" in database_layout(self.db), "Fixed size did not enable free placement")
            def by_name(name):
                return next(p for p in saved_profiles(self.db) if p["name"] == name.encode().hex())
            saved = by_name(profiles[0]["name"])
            self.assertEqual((saved["displayPreset"], saved["scale"], saved["fit"]), ("3", "50", "0"))
            self.assertEqual((saved["pixelWidth"], saved["pixelHeight"]), ("642", "426"))
            self.assertEqual(by_name(profiles[1]["name"])["scale"], "100")
            # The scaled viewport is exactly 640 x 400, plus two border pixels and 26 chrome pixels.
            self.drag(window, 60, 56, 90, 106)
            saved = by_name(profiles[0]["name"])
            self.assertEqual((saved["pixelX"], saved["pixelY"]), ("30", "50"))
            self.assertEqual((saved["pixelWidth"], saved["pixelHeight"]), ("642", "426"))
            if SCREENSHOT:
                time.sleep(0.2)
                capture_window(window, str(SCREENSHOT) + ".scaled.png")
            self.assertEqual(first.inputs + second.inputs, [], "Local size or move controls reached a panel")
        before, frames_before = first.authentications, first.frames
        with self.viewer():
            wait_for(lambda: first.authentications > before and first.frames > frames_before + 2,
                     "Scaled layout did not reconnect and display new frames")
            window = self.window()
            # Coordinates map from the 640 x 400 picture to the fixture's 320 x 200 framebuffer.
            self.click(window, 355, 319)
            wait_for(lambda: any(event[0] == "pointer" and event[1] == 1 for event in first.inputs),
                     "Restored scaled view did not accept control input")
            down = next(event for event in first.inputs if event[0] == "pointer" and event[1] == 1)
            self.assertEqual(down[2:], (160, 100))
            # Freely resizing returns this view to Fit; the other window retains its settings.
            self.drag(window, 671, 515, 731, 555)
            resized = next(p for p in saved_profiles(self.db) if p["name"] == profiles[0]["name"].encode().hex())
            self.assertEqual((resized["pixelWidth"], resized["pixelHeight"], resized["fit"]), ("702", "466", "1"))
            self.assertEqual(second.inputs, [])

    def test_arrangement_dividers_are_stable_across_resolution_reconnect_and_restart(self):
        self.require_input()
        servers = [self.server(size=size) for size in [(320, 200), (480, 270), (240, 320)]]
        create_database(self.db, [("Mixed aspect ratios", [panel_profile(server, str(index)) for index, server in enumerate(servers)])])
        with self.viewer():
            wait_for(lambda: all(server.frames > 2 for server in servers), "Panels did not stream")
            window = self.window()
            self.xdo("windowmove", window, 90, 60)
            original = self.geometry(window)
            border, gap = (220, 227, 236), (242, 245, 249)
            for preset in (4, 5, 3, 2, 6, 4):
                self.choose_preset(window, preset)
                self.assertEqual(self.geometry(window), original, "Arrangement changed the application window")
            picture = self.picture(window, ".stable-grid.png")
            self.assertEqual(self.pixel(picture, 100, 447), border)
            self.assertEqual(self.pixel(picture, 100, 450), gap)
            self.assertEqual(self.pixel(picture, 658, 500), gap)
            self.drag(window, 100, 450, 100, 350)
            self.drag(window, 660, 500, 760, 500)
            encoded = database_layout(self.db)
            self.assertIn("rowWeights=304,504", encoded)
            self.assertIn("columnWeights=754,554", encoded)
            # Reordering changes the screen in a slot, preserving the dividers.
            self.drag(window, 70, 56, 80, 364)
            self.assertIn("rowWeights=304,504", database_layout(self.db))
            servers[0].size = (640, 360)
            before = servers[0].frames
            servers[0].drop.set()
            wait_for(lambda: servers[0].authentications == 2 and servers[0].frames > before + 2,
                     "Changed resolution did not reconnect")
            picture = self.picture(window, ".dividers.png")
            self.assertEqual(self.pixel(picture, 100, 350), gap)
            self.assertEqual(self.pixel(picture, 760, 500), gap)
            self.assertEqual(self.geometry(window), original)
            self.assertEqual([event for server in servers for event in server.inputs], [])
        counts = [server.frames for server in servers]
        with self.viewer():
            wait_for(lambda: all(server.frames > before + 2 for server, before in zip(servers, counts)), "Saved grid did not reopen")
            picture = self.picture(self.window(), ".restored-grid.png")
            self.assertEqual(self.pixel(picture, 100, 350), gap)
            self.assertEqual(self.pixel(picture, 760, 500), gap)

    def test_fullscreen_hides_chrome_consumes_shortcuts_and_restores_window(self):
        self.require_input()
        servers = [self.server(size=(320, 400)), self.server(size=(320, 400))]
        create_database(self.db, [("Screens only", [panel_profile(server, str(index)) for index, server in enumerate(servers)])])
        with self.viewer():
            wait_for(lambda: all(server.frames > 2 for server in servers), "Panels did not stream")
            window = self.window()
            self.xdo("windowmove", window, 70, 80)
            self.xdo("windowsize", window, 1160, 780)
            time.sleep(0.15)
            original = self.geometry(window)
            self.click(window, 300, 420)  # Only the simulated panel receives this input.
            self.xdo("keydown", "a")
            wait_for(lambda: ("key", 1, ord("a")) in servers[0].inputs, "Fixture did not take keyboard focus")
            self.xdo("key", "F11")
            window = self.window()
            wait_for(lambda: self.geometry(window)["WIDTH"] == 1600, "Fullscreen did not cover the test display")
            self.xdo("keyup", "a")
            wait_for(lambda: ("key", 0, ord("a")) in servers[0].inputs, "Fullscreen left a remote key held")
            full = self.picture(window, ".fullscreen.png")
            self.assertEqual(full[:2], (1600, 1000))
            for x in (40, 450, 850, 1200):
                self.assertEqual(self.pixel(full, x, 12), (35, 72, 111), "Toolbar or panel header remains in fullscreen")
            self.assertEqual(self.pixel(full, 100, 998), (244, 247, 250), "Footer remains in fullscreen")
            self.click(window, 200, 500)
            self.xdo("key", "Escape")
            window = self.window()
            wait_for(lambda: self.geometry(window) == original, "Escape did not restore window bounds")
            self.assertFalse(any(event[0] == "key" and event[2] in (0xffc8, 0xff1b)
                                 for server in servers for event in server.inputs), "Fullscreen shortcuts reached a panel")
            # The visible exit button consumes its entire click, including release.
            self.click(window, 1070, 20)
            window = self.window()
            wait_for(lambda: self.geometry(window)["WIDTH"] == 1600, "Fullscreen button did not enter")
            before = sum(len(server.inputs) for server in servers)
            self.click(window, 1579, 18)
            window = self.window()
            wait_for(lambda: self.geometry(window) == original, "Exit button did not restore window bounds")
            self.assertEqual(sum(len(server.inputs) for server in servers), before)
            self.assertEqual([server.connections for server in servers], [1, 1])
            # Auto-repeat must not toggle repeatedly while the key is held.
            self.xdo("keydown", "F11")
            window = self.window()
            self.xdo("keydown", "F11")
            wait_for(lambda: self.geometry(window)["WIDTH"] == 1600, "Repeated F11 toggled back out")
            self.xdo("keyup", "F11")
            self.xdo("key", "F11")
            window = self.window()
            wait_for(lambda: self.geometry(window) == original, "F11 did not restore after repeat")
            encoded = database_layout(self.db)
            self.assertIn("width=1160", encoded)
            self.assertIn("height=780", encoded)
            self.assertNotIn("width=1600", encoded)
            self.assertNotIn("height=1000", encoded)

    def test_free_positions_and_scale_restore_after_grid_and_fullscreen(self):
        self.require_input()
        servers = [self.server(size=(320, 200)), self.server(size=(320, 200))]
        profiles = [panel_profile(server, str(index)) for index, server in enumerate(servers)]
        for index, profile in enumerate(profiles):
            profile.update(pixelX=20 + index * 665, pixelY=25 + index * 40, pixelWidth=642, pixelHeight=426,
                           displayWidth=1280, displayHeight=800, displayPreset=3, scale=50, fit=0, freePositioned=1)
        create_database(self.db, [("Preserve free views", profiles)])
        with self.viewer():
            wait_for(lambda: all(server.frames > 2 for server in servers), "Panels did not stream")
            window = self.window()
            self.choose_preset(window, 9)
            original = saved_profiles(self.db)
            for preset in (4, 3, 2, 9):
                self.choose_preset(window, preset)
                for before, after in zip(original, saved_profiles(self.db)):
                    for key in ("pixelX", "pixelY", "pixelWidth", "pixelHeight", "fit", "scale", "displayPreset"):
                        self.assertEqual(after[key], before[key], f"Arrangement changed saved {key}")
            self.xdo("key", "F11")
            window = self.window()
            wait_for(lambda: self.geometry(window)["WIDTH"] == 1600, "Free layout did not enter fullscreen")
            self.picture(window, ".free-fullscreen.png")
            self.xdo("key", "Escape")
            window = self.window()
            wait_for(lambda: self.geometry(window)["WIDTH"] == 1320, "Free layout did not restore")
            self.choose_preset(window, 2)
            self.choose_preset(window, 9)
            for before, after in zip(original, saved_profiles(self.db)):
                for key in ("pixelX", "pixelY", "pixelWidth", "pixelHeight", "fit", "scale", "displayPreset"):
                    self.assertEqual(after[key], before[key], f"Fullscreen changed saved {key}")
            self.assertEqual([event for server in servers for event in server.inputs], [])


def root_windows():
    # Menus can have no title or PID property. Query the isolated Xvfb root
    # directly so the appearance test also sees those transient windows.
    import ctypes.util
    x = ctypes.CDLL(ctypes.util.find_library("X11"))
    x.XOpenDisplay.restype = ctypes.c_void_p
    display = x.XOpenDisplay(None)
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    root, parent, count = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_uint()
    children = ctypes.POINTER(ctypes.c_ulong)()
    x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.c_void_p] * 4
    x.XFree.argtypes = [ctypes.c_void_p]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    try:
        assert x.XQueryTree(display, x.XDefaultRootWindow(display), ctypes.byref(root), ctypes.byref(parent),
                            ctypes.byref(children), ctypes.byref(count))
        return {str(children[index]) for index in range(count.value)}
    finally:
        if children:
            x.XFree(children)
        x.XCloseDisplay(display)


def capture_window(window, path):
    # Read pixels from the isolated test display. This sends no input events.
    import ctypes.util
    x = ctypes.CDLL(ctypes.util.find_library("X11"))
    x.XOpenDisplay.restype = ctypes.c_void_p
    display = x.XOpenDisplay(None)
    try:
        x.XGetGeometry.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.c_void_p] * 7
        root, xpos, ypos = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_int()
        width, height, border, depth = (ctypes.c_uint() for _ in range(4))
        x.XGetGeometry(display, int(window), ctypes.byref(root), ctypes.byref(xpos), ctypes.byref(ypos),
                       ctypes.byref(width), ctypes.byref(height), ctypes.byref(border), ctypes.byref(depth))
        class XImage(ctypes.Structure):
            _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int),
                        ("format", ctypes.c_int), ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int),
                        ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int),
                        ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                        ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int)]
        x.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                               ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
        x.XGetImage.restype = ctypes.POINTER(XImage)
        picture = x.XGetImage(display, int(window), 0, 0, width.value, height.value, 0xffffffff, 2)
        assert picture.contents.bits_per_pixel == 32
        raw = ctypes.string_at(picture.contents.data, picture.contents.bytes_per_line * height.value)
        # PNG using only the standard library (Xvfb stores BGRX pixels).
        import zlib
        def chunk(kind, data):
            return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
        rows = bytearray()
        for row in range(height.value):
            rows.append(0)
            for col in range(width.value):
                offset = row * picture.contents.bytes_per_line + col * 4
                rows.extend((raw[offset + 2], raw[offset + 1], raw[offset]))
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!IIBBBBB", width.value, height.value, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b"")
        Path(path).write_bytes(png)
        x.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
        stride = picture.contents.bytes_per_line
        x.XDestroyImage(picture)
        return width.value, height.value, raw, stride
    finally:
        x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x.XCloseDisplay(display)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--viewer", required=True)
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--input-tests", action="store_true")
    parser.add_argument("--screenshot")
    args, remaining = parser.parse_known_args()
    VIEWER = str(Path(args.viewer).resolve())
    protocol.OPENSSL = args.openssl
    INPUT_TESTS, SCREENSHOT = args.input_tests, args.screenshot
    unittest.main(argv=[__file__, *remaining], verbosity=2)
