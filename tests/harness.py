"""The scenario harness (docs/PLAN.md phase 2): starts a simulated implement, drives it with
commands over its TCP port, reads its events, and checks what happened. For scenarios on a bus
it also starts AOG-TaskController headless and plays AgOpenGPS on its UDP link.

    from harness import Implement, TaskController, App, check, summary
"""
import json
import os
import select
import socket
import struct
import subprocess
import tempfile
import time

_results = []


def check(cond, what):
    """Records and prints one check."""
    _results.append((bool(cond), what))
    print(("ok    " if cond else "FAIL  ") + what, flush=True)
    return bool(cond)


def note(what):
    print("note  " + what, flush=True)


def summary():
    """Prints PASS or FAILED with the count; returns the exit code."""
    failed = [w for c, w in _results if not c]
    if failed:
        print("FAILED: %d of %d checks" % (len(failed), len(_results)))
        return 1
    print("PASS: %d checks" % len(_results))
    return 0


def bits(n, count):
    """Section bits as a string, section 1 first."""
    return "".join("1" if (n >> i) & 1 else "0" for i in range(count))


def profile_sections(path):
    """(booms, total sections, delay on, delay off) from a profile file."""
    with open(path) as f:
        prof = json.load(f)
    count = 0
    for b in prof["booms"]:
        if isinstance(b.get("sections"), list):
            count += len(b["sections"])
        elif "sections" in b:
            count += b["sections"]["count"]
        else:
            count += b["rows"]["count"]
    d = prof.get("sectionDelay", {})
    return len(prof["booms"]), count, d.get("on", 0.3), d.get("off", 0.2)


class Implement:
    """One simulated implement: the binary with a profile, its events, and a TCP command link."""

    def __init__(self, binary, profile, iface=None, driver=None, extra=()):
        args = [binary, "--profile", profile, "--port", "0", *extra]
        if iface:
            args += ["--interface", iface]
        if driver:
            args += ["--driver", driver]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.set_blocking(self.proc.stdout.fileno(), False)
        os.set_blocking(self.proc.stderr.fileno(), False)
        self.buf = b""
        self.err = b""
        self.events = []
        self.tcp_events = []
        self.sock = None
        self.sockbuf = b""
        self.connected = False
        self.sections = {}
        self.port = None
        # The port comes in the "listening" event on stdout
        deadline = time.time() + 10
        while time.time() < deadline and self.port is None:
            self.poll()
            time.sleep(0.02)
        if self.port is None:
            self.stop()
            raise RuntimeError("the implement did not start: " + self.err.decode(errors="replace")[-500:])
        self.sock = socket.create_connection(("127.0.0.1", self.port))
        self.sock.setblocking(False)

    def _take(self, e):
        self.events.append(e)
        if e["event"] == "listening":
            self.port = e["port"]
        elif e["event"] == "tc":
            self.connected = e["connected"]
        elif e["event"] == "sections":
            self.sections[e["boom"]] = e

    def poll(self):
        fds = [self.proc.stdout, self.proc.stderr] + ([self.sock] if self.sock else [])
        r, _, _ = select.select(fds, [], [], 0)
        if self.proc.stderr in r:
            self.err += self.proc.stderr.read() or b""
        if self.proc.stdout in r:
            self.buf += self.proc.stdout.read() or b""
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                try:
                    self._take(json.loads(line))
                except ValueError:
                    pass
        if self.sock and self.sock in r:
            try:
                data = self.sock.recv(65536)
            except BlockingIOError:
                data = b""
            if data:
                self.sockbuf += data
                while b"\n" in self.sockbuf:
                    line, self.sockbuf = self.sockbuf.split(b"\n", 1)
                    try:
                        e = json.loads(line)
                    except ValueError:
                        continue
                    e["_tcp"] = True
                    self.tcp_events.append(e)

    def send(self, command, timeout=2.0):
        """Sends one command over TCP; returns its "command" event (from the TCP stream), or None."""
        n = len(self.tcp_events)
        self.sock.sendall((command + "\n").encode())
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.poll()
            for e in self.tcp_events[n:]:
                if e["event"] == "command" and e.get("command") == command:
                    return e
            time.sleep(0.005)
        return None

    def status(self):
        e = self.send("status")
        return e["status"] if e and e.get("ok") else None

    def wait(self, pred, timeout, tick=None):
        """Polls until pred(self) is true. Returns the seconds it took, or None."""
        start = time.time()
        while time.time() - start < timeout:
            if tick:
                tick()
            self.poll()
            if pred(self):
                return time.time() - start
            time.sleep(0.01)
        return None

    def actual(self, boom=0):
        e = self.sections.get(boom)
        return e["actual"] if e else None

    def find(self, name, since=0, **match):
        """Events of a name since an index, with matching fields."""
        return [e for e in self.events[since:] if e["event"] == name and all(e.get(k) == v for k, v in match.items())]

    def stop(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.proc.poll() is None:
            self.proc.send_signal(2)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.poll()


TC_PORT = 8888
APP_PORT = 9999


def _frame(pgn, data, src=0x7F):
    body = bytes([src, pgn, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


class App:
    """What AgOpenGPS sends AOG-TaskController, at 10 Hz: sections in PGN 229, speed in 242,
    section control in 241 (on change and every 2 s: see the parser note in PLAN.md phase 1)."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", APP_PORT))
        self.sock.setblocking(False)
        self.sections = 0
        self.speed_mm_s = 2778
        self.control = 1
        self.status = None  # last PGN 240: (mode, count, bits)
        self.last_send = 0
        self.last_control = (None, 0)

    def tick(self):
        try:
            while True:
                d = self.sock.recv(2048)
                if len(d) >= 7 and d[:2] == b"\x80\x81" and d[3] == 0xF0:
                    n = d[4]
                    payload = d[5:5 + n]
                    if n >= 2:
                        self.status = (payload[0], payload[1], int.from_bytes(payload[2:], "little"))
        except BlockingIOError:
            pass
        now = time.time()
        if now - self.last_send < 0.1:
            return
        self.last_send = now
        to = ("127.0.0.1", TC_PORT)
        self.sock.sendto(_frame(0xE5, struct.pack("<Q", self.sections & 0xFFFFFFFFFFFFFFFF)), to)
        self.sock.sendto(_frame(0xF2, struct.pack("<Hi", 397, self.speed_mm_s)), to)
        if self.control != self.last_control[0] or now - self.last_control[1] >= 2.0:
            self.sock.sendto(_frame(0xF1, [self.control]), to)
            self.last_control = (self.control, now)


class TaskController:
    """AOG-TaskController headless on a SocketCAN interface, logging to a file."""

    def __init__(self, binary, iface):
        self.cfgdir = tempfile.mkdtemp(prefix="tc-")
        env = dict(os.environ, XDG_CONFIG_HOME=self.cfgdir)
        os.makedirs(os.path.join(self.cfgdir, "AOG-TaskController"), exist_ok=True)
        with open(os.path.join(self.cfgdir, "AOG-TaskController", "settings.json"), "w") as f:
            json.dump({"subnet": [127, 0, 0], "tecuEnabled": True, "nmeaSendEnabled": False,
                       "aogHeartbeatEnabled": True, "vtEnabled": False, "tcVersion": 3}, f)
        self.logpath = os.path.join(self.cfgdir, "tc.log")
        self.log = open(self.logpath, "w")
        self.proc = subprocess.Popen([binary, "--can_adapter=socketcan", "--can_channel=" + iface],
                                     stdout=self.log, stderr=subprocess.STDOUT, env=env)
        self.pos = 0

    def new_lines(self):
        with open(self.logpath) as f:
            f.seek(self.pos)
            text = f.read()
            self.pos = f.tell()
        return text.splitlines()

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
