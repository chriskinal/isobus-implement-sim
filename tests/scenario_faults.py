#!/usr/bin/env python3
"""Phase 4 (docs/PLAN.md section 7): the faults, each against AOG-TaskController and the headless
terminal on vcan0. What a fault should do to the implement is checked; what the task controller
and the terminal make of it is recorded, since that is what the fault is for.

    tests/scenario_faults.py --tc ~/aog-tc-build-new/AOG-TaskController --implement build-linux/implement --stub build-linux/vt_stub
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from harness import App, Implement, TaskController, VtStub, bits, check, note, summary  # noqa: E402

MAIN_MASK = 1000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tc", default=os.path.expanduser("~/aog-tc-build-new/AOG-TaskController"))
    ap.add_argument("--implement", default="build-linux/implement")
    ap.add_argument("--stub", default="build-linux/vt_stub")
    ap.add_argument("--iface", default="vcan0")
    ap.add_argument("--profile", default="profiles/sprayer-24.json")
    args = ap.parse_args()

    app = App()
    tc = TaskController(args.tc, args.iface)
    vt = VtStub(args.stub, args.iface)
    imp = None

    def wait(seconds, cond):
        start = time.time()
        while time.time() - start < seconds:
            app.tick()
            vt.poll()
            if imp:
                imp.poll()
            if cond():
                return time.time() - start
            time.sleep(0.01)
        return None

    def sections_follow(pattern, timeout, label):
        app.sections = pattern
        t = wait(timeout, lambda: imp.actual() == bits(pattern, 12))
        check(t is not None, "%s: sections follow %s (%s)" % (label, bits(pattern, 12), "%.2f s" % t if t else imp.actual()))
        return t is not None

    def tc_notes(keys, limit=6):
        shown = 0
        for line in tc.new_lines():
            if any(k in line for k in keys) and shown < limit:
                note("  TC: " + line.strip()[:160])
                shown += 1

    try:
        t = wait(15.0, lambda: app.status is not None and vt.find("ready"))
        check(t is not None, "task controller and terminal up")
        imp = Implement(args.implement, args.profile, iface=args.iface)
        t = wait(45.0, lambda: imp.connected and imp.find("vt", connected=True))
        check(t is not None, "the implement connects to both (%s)" % ("%.1f s" % t if t else "never"))
        sections_follow(0b101010101010, 4.0, "baseline")
        tc.new_lines()

        # slow: every frame held 150 ms; the session survives, the delay shows
        e = imp.send("fault slow 150")
        check(e and e["ok"], "fault slow 150")
        sections_follow(0b000000001111, 5.0, "slow 150 ms")
        e = imp.send("fault slow 0")
        check(e and e["ok"] and imp.connected, "fault slow 0: still connected")

        # silent 3 s: shorter than the task controller's timeout; nothing is lost
        n = len(imp.events)
        e = imp.send("fault silent 3")
        check(e and e["ok"], "fault silent 3")
        app.sections = 0b111100000000
        wait(3.5, lambda: False)
        dropped = any(e2["event"] == "tc" and not e2["connected"] for e2 in imp.events[n:])
        note("  during 3 s of silence the implement's view of the TC: %s; TC heartbeat count %s" % ("lost" if dropped else "kept", app.status))
        t = wait(6.0, lambda: imp.actual() == bits(0b111100000000, 12))
        check(t is not None, "after 3 s silent the sections catch up (%s)" % ("%.2f s" % t if t else imp.actual()))
        tc_notes(("timed out", "timeout", "registered"))

        # silent 12 s: longer than its timeout; the task controller drops the client; what then is recorded
        n = len(imp.events)
        e = imp.send("fault silent 12")
        check(e and e["ok"], "fault silent 12")
        t = wait(12.0, lambda: app.status is not None and app.status[1] == 0)
        note("  TC dropped the implement after %s of silence" % ("%.1f s" % t if t else "never (heartbeat %s)" % (app.status,)))
        tc_notes(("timed out", "timeout"))
        wait(2.0, lambda: False)
        app.sections = 0b000000111111
        t = wait(15.0, lambda: imp.actual() == bits(0b000000111111, 12))
        note("  after the silence, without a new handshake: sections %s (%s); implement tc events: %s" % (
            "follow again" if t else "do not follow", "%.1f s" % t if t else imp.actual(),
            ", ".join("%s@%d" % (e2["connected"], e2["t"]) for e2 in imp.find("tc", since=n))))
        tc_notes(("registered", "Attempting", "timed out"))

        # restart: leave and rejoin; both sessions come back; the terminal has the pool stored
        n = len(imp.events)
        nv = len(vt.events)
        e = imp.send("fault restart")
        check(e and e["ok"], "fault restart")
        t = wait(45.0, lambda: imp.find("tc", since=n, connected=True))
        check(t is not None, "the task controller takes the implement back (%s)" % ("%.1f s" % t if t else "never"))
        t = wait(20.0, lambda: imp.find("vt", since=n, connected=True))
        check(t is not None, "the terminal takes it back (%s)" % ("%.1f s" % t if t else "never"))
        loads = vt.find("loadVersion", since=nv)
        note("  terminal: loadVersion %s; pool events %s" % (
            ", ".join("%s found=%s" % (l["label"], l["found"]) for l in loads) or "none",
            ", ".join("%s fromMemory=%s" % (p["state"], p.get("fromMemory")) for p in vt.find("pool", since=nv))))
        sections_follow(0b110000000011, 6.0, "after restart")
        tc_notes(("registered", "Attempting"))

        # version: the pool again under another label; the terminal stores it as new
        nv = len(vt.events)
        n = len(imp.events)
        e = imp.send("fault version v2test")
        check(e and e["ok"], "fault version v2test")
        t = wait(30.0, lambda: imp.find("vt", since=n, connected=True))
        check(t is not None, "terminal connected again under the new label")
        saves = vt.find("saveVersion", since=nv)
        check(any(s["label"].startswith("v2test") for s in saves), "the terminal stored label v2test: %s" % [s["label"] for s in saves])
        loads = vt.find("loadVersion", since=nv)
        note("  loadVersion before the upload: %s" % (", ".join("%s found=%s" % (l["label"], l["found"]) for l in loads) or "none"))

        # bigpool: 200 kB more; the terminal must take it
        nv = len(vt.events)
        n = len(imp.events)
        e = imp.send("fault bigpool 200")
        check(e and e["ok"], "fault bigpool 200")
        t = wait(120.0, lambda: imp.find("vt", since=n, connected=True))
        check(t is not None, "a 240 kB pool uploads and the terminal connects (%s)" % ("%.1f s" % t if t else "never"))
        pools = vt.find("pool", since=nv)
        saved = [s2["bytes"] for s2 in vt.find("saveVersion", since=nv)]
        check(saved and max(saved) >= 39826 + 200 * 1024, "the terminal stored the padded pool: %s bytes" % saved)
        note("  terminal pool events: %s" % ", ".join(p["state"] for p in pools))

        # ddop bad: a corrupted DDOP; the task controller must refuse it and stay alive
        n = len(imp.events)
        tc.new_lines()
        e = imp.send("fault ddop bad")
        check(e and e["ok"], "fault ddop bad")
        wait(25.0, lambda: any(e2["state"] in ("connected", "disconnected") for e2 in imp.find("tcState", since=n)[1:]))
        note("  implement tc states after the bad DDOP: %s" % ", ".join(e2["state"] for e2 in imp.find("tcState", since=n)))
        tc_notes(("Failed", "fail", "error", "Error", "activat", "registered", "deserializ"), limit=8)
        check(tc.proc.poll() is None, "the task controller is still running")
        n = len(imp.events)
        imp.send("fault restart")
        t = wait(45.0, lambda: imp.find("tc", since=n, connected=True))
        check(t is not None, "after a restart with the good DDOP the task controller takes it back")
        sections_follow(0b101010101010, 6.0, "after the bad DDOP")

        # busoff: error frames on the bus; everyone carries on
        e = imp.send("fault busoff 50")
        check(e and e["ok"], "fault busoff 50: %s frames written" % (e and e.get("sent")))
        sections_follow(0b111111111111, 6.0, "after the error frames")
        check(tc.proc.poll() is None and vt.proc.poll() is None, "task controller and terminal still running")

        # conflict: a second device claims the task controller's address; what each stack does is recorded
        n = len(imp.events)
        tc.new_lines()
        before = imp.status()
        e = imp.send("fault conflict")
        check(e and e["ok"], "fault conflict: claim sent for address %s by identity %s" % ((e and imp.find("conflict", since=n)[0]["address"]), (e and imp.find("conflict", since=n)[0]["contenderIdentity"])))
        wait(3.0, lambda: False)
        after = imp.status()
        note("  implement address %s -> %s; implement's view of the TC %s -> %s" % (before["address"], after["address"], before["tc"]["connected"], after["tc"]["connected"]))
        tc_notes(("address", "Address", "claim", "NM"))
        app.sections = 0b000011110000
        t = wait(10.0, lambda: imp.actual() == bits(0b000011110000, 12))
        note("  after the conflict the sections %s (%s)" % ("follow" if t else "do not follow", "%.1f s" % t if t else imp.actual()))
        if not t:
            n = len(imp.events)
            imp.send("fault restart")
            t = wait(45.0, lambda: imp.find("tc", since=n, connected=True))
            check(t is not None, "after a restart the task controller takes it back")
            sections_follow(0b000011110000, 6.0, "after the conflict and a restart")
        check(tc.proc.poll() is None and vt.proc.poll() is None, "task controller and terminal still running at the end")
    finally:
        if imp:
            imp.stop()
            if imp.proc.returncode not in (0, -2, None):
                note("implement stderr: " + imp.err.decode(errors="replace")[-500:])
        vt.stop()
        tc.stop()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
