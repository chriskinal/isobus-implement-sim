#!/usr/bin/env python3
"""Phase 1 (docs/PLAN.md section 8): the shipped profiles connect to AOG-TaskController on a
virtual CAN bus and their sections follow its setpoints, with the profile's delay.

Runs on Linux (the OrbStack machine, or CI): AOG-TaskController headless on vcan0, this script
as the app on its UDP link (sections in PGN 229, section control in 241, speed in 242), and the
simulator with one profile after another.

    sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
    tests/phase1_tc.py --tc ~/aog-tc-build/AOG-TaskController --implement build-linux/implement
"""
import argparse
import json
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

TC_PORT = 8888   # the task controller listens here (loopback when no LAN matches its subnet)
APP_PORT = 9999  # it broadcasts to <subnet>.255:9999


def frame(pgn, data, src=0x7F):
    body = bytes([src, pgn, len(data)]) + bytes(data)
    return b"\x80\x81" + body + bytes([sum(body) & 0xFF])


def bits(n, count):
    return "".join("1" if (n >> i) & 1 else "0" for i in range(count))


class App:
    """What AgOpenGPS sends the task controller, at 10 Hz."""
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", APP_PORT))
        self.sock.setblocking(False)
        self.sections = 0
        self.speed_mm_s = 2778  # 10 km/h
        self.control = 1
        self.status = None  # last PGN 240: (mode, count, bits)
        self.last_send = 0

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
        self.sock.sendto(frame(0xE5, struct.pack("<Q", self.sections & 0xFFFFFFFFFFFFFFFF)), to)
        self.sock.sendto(frame(0xF1, [self.control]), to)
        # AOG-TaskController reads speed from DDI 397 (ActualSpeed) in app.cpp; its PROTOCOL.md says 156
        self.sock.sendto(frame(0xF2, struct.pack("<Hi", 397, self.speed_mm_s)), to)


class Implement:
    def __init__(self, binary, profile, iface, extra=()):
        self.proc = subprocess.Popen([binary, "--profile", profile, "--interface", iface, *extra],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        os.set_blocking(self.proc.stdout.fileno(), False)
        os.set_blocking(self.proc.stderr.fileno(), False)
        self.buf = b""
        self.err = b""
        self.events = []
        self.connected = False
        self.tc = None
        self.sections = {}  # boom -> last sections event

    def poll(self):
        r, _, _ = select.select([self.proc.stdout, self.proc.stderr], [], [], 0)
        if self.proc.stderr in r:
            self.err += self.proc.stderr.read() or b""
        if self.proc.stdout in r:
            self.buf += self.proc.stdout.read() or b""
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            try:
                e = json.loads(line)
            except ValueError:
                continue
            self.events.append(e)
            if e["event"] == "tc":
                self.connected = e["connected"]
                self.tc = e
            elif e["event"] == "sections":
                self.sections[e["boom"]] = e

    def actual(self, boom=0):
        e = self.sections.get(boom)
        return e["actual"] if e else None

    def stop(self):
        self.proc.send_signal(2)
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


class Tc:
    def __init__(self, binary, iface, cfgdir):
        env = dict(os.environ, XDG_CONFIG_HOME=cfgdir)
        os.makedirs(os.path.join(cfgdir, "AOG-TaskController"), exist_ok=True)
        with open(os.path.join(cfgdir, "AOG-TaskController", "settings.json"), "w") as f:
            json.dump({"subnet": [127, 0, 0], "tecuEnabled": True, "nmeaSendEnabled": False,
                       "aogHeartbeatEnabled": True, "vtEnabled": False, "tcVersion": 3}, f)
        self.logpath = os.path.join(cfgdir, "tc.log")
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tc", default=os.path.expanduser("~/aog-tc-build/AOG-TaskController"))
    ap.add_argument("--implement", default="build-linux/implement")
    ap.add_argument("--iface", default="vcan0")
    ap.add_argument("--profiles", default="profiles")
    ap.add_argument("--only", nargs="*", help="profile names to run, e.g. planter-12 sprayer-24")
    args = ap.parse_args()

    for binary in (args.tc, args.implement):
        if not os.access(binary, os.X_OK):
            print("FAIL  no executable at %s" % binary)
            return 2
    cfgdir = tempfile.mkdtemp(prefix="phase1-")
    app = App()
    tc = Tc(args.tc, args.iface, cfgdir)
    ok = True
    results = []

    def check(cond, what):
        nonlocal ok
        ok = ok and bool(cond)
        print(("ok    " if cond else "FAIL  ") + what, flush=True)
        results.append((bool(cond), what))

    def note(what):
        print("note  " + what, flush=True)

    def run(seconds, imp=None, until=None):
        """Plays the app and polls the implement. Returns the time until the condition held, or None."""
        start = time.time()
        while time.time() - start < seconds:
            app.tick()
            if imp:
                imp.poll()
            if until and until():
                return time.time() - start
            time.sleep(0.01)
        return None if until else seconds

    def tc_lines():
        return tc.new_lines()

    # The shipped profiles, with what each should show (docs/SEEDER.md and SPRAYER.md section 8)
    plan = [
        ("planter-12", [], True), ("planter-12", ["--section-control", "actualSettable"], True),
        ("planter-12", ["--section-control", "perElement"], True),
        ("planter-24", [], True), ("planter-48", [], True), ("drill-6m", [], True),
        ("sprayer-24", [], True), ("sprayer-mounted-12", [], True), ("sprayer-120ft", [], True),
        ("airseeder-12m", [], False), ("sprayer-36-nozzle", [], False),
        ("planter-12", [], True),  # the task controller still works after the odd ones
    ]
    if args.only:
        plan = [p for p in plan if p[0] in args.only]

    try:
        # The task controller comes up and sends its empty heartbeat
        t = run(15.0, until=lambda: app.status is not None)
        check(t is not None, "AOG-TaskController heartbeat on UDP (%s)" % ("%.1f s" % t if t else "never"))
        tc_lines()

        for name, extra, expected in plan:
            with open(os.path.join(args.profiles, name + ".json")) as f:
                prof = json.load(f)
            booms = prof["booms"]
            count = sum(len(b["sections"]) if isinstance(b.get("sections"), list)
                        else (b["sections"]["count"] if "sections" in b else b["rows"]["count"]) for b in booms)
            delay_on = prof.get("sectionDelay", {}).get("on", 0.3)
            delay_off = prof.get("sectionDelay", {}).get("off", 0.2)
            label = name + ("" if not extra else " " + extra[-1])
            print("----- %s: %d boom(s), %d sections, delay %.2f/%.2f s" % (label, len(booms), count, delay_on, delay_off), flush=True)
            app.sections = 0
            imp = Implement(args.implement, os.path.join(args.profiles, name + ".json"), args.iface, extra)
            try:
                t = run(45.0, imp, lambda: imp.connected)
                if not expected:
                    note("%s: connected=%s after %s" % (label, imp.connected, "%.1f s" % t if t else "45 s"))
                    for line in tc_lines():
                        if any(k in line for k in ("registered", "Section Control via", "boom", "WARNING", "Failed", "error", "Error")):
                            note("  TC: " + line.strip())
                    if imp.connected:
                        app.sections = (1 << count) - 1
                        n_events = len(imp.events)
                        run(delay_on + 3.0, imp)
                        note("  implement actual: %s" % " | ".join(imp.actual(b) or "-" for b in range(len(booms))))
                        note("  TC PGN 240: %s" % (app.status,))
                        sets = [e for e in imp.events[n_events:] if e["event"] == "set"]
                        note("  TC sent: %s" % ", ".join("el%s %s=%s" % (e["element"], e.get("what", e["ddi"]), e["value"]) for e in sets[:12]))
                    continue
                check(t is not None, "%s connects (%s)" % (label, "%.1f s" % t if t else "never"))
                if not imp.connected:
                    note("  implement stderr tail: " + imp.err.decode(errors="replace")[-400:])
                    continue
                registered = [l for l in tc_lines() if "registered successfully" in l or "Section Control via" in l]
                for line in registered:
                    note("  TC: " + line.strip())
                check(any(("with %d sections" % count) in l for l in registered), "TC registered %d sections" % count)

                # Alternate sections on: actual follows after the on delay
                pattern = sum(1 << i for i in range(0, count, 2))
                app.sections = pattern
                t = run(delay_on + 4.0, imp, lambda: imp.actual() == bits(pattern, count))
                check(t is not None, "sections follow the setpoint %s (%s)" % (bits(pattern, count), "%.2f s" % t if t else imp.actual()))
                # The delay, from the implement's own event clock: setpoint seen to actual seen
                sp = [e for e in imp.events if e["event"] == "sections" and e["boom"] == 0 and e["setpoint"] == bits(pattern, count)]
                act = [e for e in imp.events if e["event"] == "sections" and e["boom"] == 0 and e["actual"] == bits(pattern, count)]
                if sp and act:
                    dt = (act[0]["t"] - sp[0]["t"]) / 1000.0
                    check(delay_on - 0.03 <= dt <= delay_on + 0.07, "on delay %.3f s (profile %.2f)" % (dt, delay_on))
                t = run(3.0, imp, lambda: app.status is not None and app.status[1] == count and bits(app.status[2], count) == bits(pattern, count))
                check(t is not None, "TC PGN 240 reports the implement's actual states (%s)" % (app.status,))

                # All on, then all off: the off delay
                app.sections = (1 << count) - 1
                run(delay_on + 3.0, imp, lambda: imp.actual() == "1" * count)
                check(imp.actual() == "1" * count, "all on: %s" % imp.actual())
                app.sections = 0
                t = run(delay_off + 4.0, imp, lambda: imp.actual() == "0" * count)
                check(t is not None, "all off (%s)" % ("%.2f s" % t if t else imp.actual()))
                sp = [e for e in imp.events if e["event"] == "sections" and e["boom"] == 0 and e["setpoint"] == "0" * count and e["actual"] == "1" * count]
                act = [e for e in imp.events if e["event"] == "sections" and e["boom"] == 0 and e["actual"] == "0" * count and e["t"] > (sp[0]["t"] if sp else 0)]
                if sp and act:
                    dt = (act[0]["t"] - sp[0]["t"]) / 1000.0
                    check(delay_off - 0.03 <= dt <= delay_off + 0.07, "off delay %.3f s (profile %.2f)" % (dt, delay_off))

                # Section control off at the app: the TC puts the implement in manual (DDI 160 = 0),
                # where the operator's switches rule and default on (MODEL.md section 2)
                app.sections = pattern
                run(delay_on + 2.0, imp, lambda: imp.actual() == bits(pattern, count))
                n_events = len(imp.events)
                app.control = 0
                t = run(4.0, imp, lambda: any(e["event"] == "mode" and not e["auto"] for e in imp.events[n_events:]))
                check(t is not None, "section control off: the TC sends DDI 160 = 0 and the implement goes to manual")
                run(delay_on + 1.0, imp)
                note("  in manual the sections are the operator's switches: %s" % imp.actual())
                sets = [e for e in imp.events[n_events:] if e["event"] == "set"]
                note("  TC sent while switching off: %s" % ", ".join("%s=%s" % (e.get("what", e["ddi"]), e["value"]) for e in sets[:8]))
                app.control = 1
                t = run(delay_on + 4.0, imp, lambda: imp.actual() == bits(pattern, count))
                check(t is not None, "section control on: auto again, sections back to the setpoint")

                # Speed reaches the implement from the TC's tractor ECU
                sp = [e for e in imp.events if e["event"] == "speed" and e.get("known")]
                check(sp and abs(sp[-1]["kmh"] - 10.0) < 0.2, "speed from the bus: %s" % (sp[-1]["kmh"] if sp else "none"))
            finally:
                app.sections = 0
                imp.stop()
                if imp.proc.returncode not in (0, -2, None):
                    note("  implement exit %s: %s" % (imp.proc.returncode, imp.err.decode(errors="replace")[-300:]))
            # The task controller notices the implement is gone before the next one
            t = run(20.0, until=lambda: app.status is not None and app.status[1] == 0)
            check(t is not None, "TC drops the implement (%s)" % ("%.1f s" % t if t else "never"))
            tc_lines()
    finally:
        tc.stop()
    failed = [w for c, w in results if not c]
    print("PASS" if ok else "FAILED: %d of %d checks (TC log: %s)" % (len(failed), len(results), tc.logpath))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
