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
    def __init__(self, fixture, password="Test123!", standard=False, hold=False, reject=False, size=(320, 180)):
        self.fixture, self.password, self.standard, self.hold, self.reject = fixture, password, standard, hold, reject
        self.connections, self.frames, self.authentications = 0, 0, 0
        self.inputs, self.errors = [], []
        self.size = size
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
                    pixels.extend((*rgb, 0))
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
        window = self.xdo("search", "--all", "--sync", "--onlyvisible", "--pid", self.process.pid,
                          "--name", "SuperSmartClient - Panel workspace").splitlines()[0]
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
        self.xdo("mousemove", "--window", window, x2, y2)
        self.xdo("mouseup", 1)
        time.sleep(0.2)

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
            self.drag(window, 70, 118, 720, 118)
            encoded = database_layout(self.db)
            self.assertLess(encoded.index(profiles[1]["id"]), encoded.index(profiles[0]["id"]))
            self.drag(window, 649, 356, 1300, 356)
            encoded = database_layout(self.db)
            self.assertIn("columns=2", encoded.split("[panel ")[1])
            self.assertEqual(first.inputs + second.inputs, [], "Grid gestures reached a panel")
            self.click(window, 70, 118)
            self.xdo("click", "--window", window, "--repeat", 2, "--delay", 100, 1)
            time.sleep(0.2)
            self.click(window, 650, 80)  # Back to grid
            # Select the next named layout. The saved startup flag is false;
            # opening the layout must nevertheless connect the whole group.
            self.click(window, 430, 30)
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
            self.click(window, 333, 240)
            self.xdo("key", "--window", window, "a")
            wait_for(lambda: ("key", 1, ord("a")) in control.inputs and ("key", 0, ord("a")) in control.inputs,
                     "Control tile did not forward key press/release")
            self.assertTrue(any(event[0] == "pointer" and event[1] & 1 for event in control.inputs))
            self.click(window, 988, 240)
            self.xdo("key", "--window", window, "b")
            time.sleep(0.3)
            self.assertEqual(monitor.inputs, [], "Monitor tile forwarded input")

    def test_add_panel_form_saves_password_and_reopens(self):
        self.require_input()
        server = self.server()
        create_database(self.db, [("New workspace", [])])
        with self.viewer():
            window = self.window()
            self.click(window, 1250, 29)
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
            self.click(window, 615, 29)
            # Open the second menu entry, Save layout as.
            self.xdo("key", "Down", "Return")
            self.xdo("type", "--delay", 1, "Saved copy")
            self.xdo("key", "Return")
            def copied():
                with contextlib.closing(sqlite3.connect(self.db)) as db:
                    return db.execute("SELECT count(*) FROM layouts WHERE name='Saved copy'").fetchone()[0] == 1
            wait_for(copied, "Layout menu did not save a named copy")

    def test_presets_reorder_into_fixed_slots_without_remote_input(self):
        self.require_input()
        servers = [self.server() for _ in range(3)]
        profiles = [panel_profile(server, name) for server, name in zip(servers, ["North", "South", "Utilities"])]
        create_database(self.db, [("Three panels", profiles)])
        with self.viewer():
            wait_for(lambda: all(server.frames > 2 for server in servers), "Panels did not stream")
            window = self.window()
            self.click(window, 160, 80)
            self.xdo("key", "Down", "Down", "Down", "Down", "Return")
            wait_for(lambda: "preset=4\n" in database_layout(self.db), "Three-panel preset was not saved")
            saved = saved_profiles(self.db)
            self.assertEqual([p["columns"] for p in saved], ["2", "1", "1"])
            if SCREENSHOT:
                time.sleep(0.2)
                capture_window(window, str(SCREENSHOT) + ".three.png")
            # Reordering keeps the preset shape, moving a different panel into the top slot.
            self.drag(window, 70, 118, 80, 487)
            saved = saved_profiles(self.db)
            self.assertEqual(saved[0]["name"], profiles[1]["name"].encode().hex())
            self.assertEqual([p["columns"] for p in saved], ["2", "1", "1"])
            self.click(window, 70, 118)
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
            self.click(window, 640, 118)  # First tile's local menu.
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
            self.assertEqual((saved["pixelWidth"], saved["pixelHeight"]), ("644", "451"))
            self.assertEqual(by_name(profiles[1]["name"])["scale"], "100")
            # The scaled viewport is exactly 640 x 400, plus four border pixels and 51 chrome pixels.
            self.drag(window, 60, 118, 90, 168)
            saved = by_name(profiles[0]["name"])
            self.assertEqual((saved["pixelX"], saved["pixelY"]), ("30", "50"))
            self.assertEqual((saved["pixelWidth"], saved["pixelHeight"]), ("644", "451"))
            if SCREENSHOT:
                time.sleep(0.2)
                capture_window(window, str(SCREENSHOT) + ".scaled.png")
            self.assertEqual(first.inputs + second.inputs, [], "Local size or move controls reached a panel")
        before = first.authentications
        with self.viewer():
            wait_for(lambda: first.authentications > before and first.frames > 4, "Scaled layout did not reconnect")
            window = self.window()
            # Coordinates map from the 640 x 400 picture to the fixture's 320 x 200 framebuffer.
            self.click(window, 360, 385)
            wait_for(lambda: any(event[0] == "pointer" and event[1] == 1 for event in first.inputs),
                     "Restored scaled view did not accept control input")
            down = next(event for event in first.inputs if event[0] == "pointer" and event[1] == 1)
            self.assertEqual(down[2:], (160, 100))
            # Freely resizing returns this view to Fit; the other window retains its settings.
            self.drag(window, 674, 596, 734, 636)
            resized = next(p for p in saved_profiles(self.db) if p["name"] == profiles[0]["name"].encode().hex())
            self.assertEqual((resized["pixelWidth"], resized["pixelHeight"], resized["fit"]), ("704", "491", "1"))
            self.assertEqual(second.inputs, [])


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
        x.XDestroyImage(picture)
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
