#!/usr/bin/env python3
"""The phase 1 checks as a scenario (docs/PLAN.md phases 1 and 2): every shipped profile against
AOG-TaskController headless on a virtual CAN bus; sections follow its setpoints with the
profile's delay, the operator's commands override them, speed comes from its tractor ECU.

The task controller is restarted for every profile, so no profile's result depends on an
earlier one (it keeps a timed-out client's uploaded pool chunks: PLAN.md phase 1 record). The
back-to-back case on one task controller is run once at the end and recorded. Build the task
controller against the stack this repository fetches: its own pin has the DataSpan bug that
scrambles its DDI-to-element map (same record).

    sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
    tests/scenario_tc.py --tc ~/aog-tc-build/AOG-TaskController --implement build-linux/implement
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from harness import App, Implement, TaskController, bits, check, note, profile_sections, summary  # noqa: E402

PLAN = [
    ("planter-12", []), ("planter-12", ["--section-control", "actualSettable"]),
    ("planter-12", ["--section-control", "perElement"]),
    ("planter-24", []), ("planter-48", []), ("drill-6m", []),
    ("sprayer-24", []), ("sprayer-mounted-12", []), ("sprayer-120ft", []),
    ("spreader-9m", []),
]
RECORDED = [("airseeder-12m", []), ("sprayer-36-nozzle", [])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tc", default=os.path.expanduser("~/aog-tc-build/AOG-TaskController"))
    ap.add_argument("--implement", default="build-linux/implement")
    ap.add_argument("--iface", default="vcan0")
    ap.add_argument("--profiles", default="profiles")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--keep-log", help="append every task controller log here")
    args = ap.parse_args()
    for binary in (args.tc, args.implement):
        if not os.access(binary, os.X_OK):
            print("FAIL  no executable at %s" % binary)
            return 2

    app = App()
    logs = []

    def tc_start():
        tc = TaskController(args.tc, args.iface)
        logs.append(tc.logpath)
        app.status = None
        app.sections = 0
        app.control = 1
        t = wait(None, 15.0, lambda: app.status is not None)
        check(t is not None, "AOG-TaskController up, heartbeat on UDP (%s)" % ("%.1f s" % t if t else "never"))
        return tc

    def wait(imp, seconds, cond):
        """Plays the app at 10 Hz and polls the implement until cond() holds; seconds it took, or None."""
        start = time.time()
        while time.time() - start < seconds:
            app.tick()
            if imp:
                imp.poll()
            if cond():
                return time.time() - start
            time.sleep(0.01)
        return None

    def tc_log_notes(tc, keys):
        for line in tc.new_lines():
            if any(k in line for k in keys):
                note("  TC: " + line.strip())

    def delay_between(imp, setpoint, actual, after=0):
        sp = [e for e in imp.find("sections", boom=0) if e["setpoint"] == setpoint and e["t"] >= after]
        act = [e for e in imp.find("sections", boom=0) if e["actual"] == actual and sp and e["t"] >= sp[0]["t"]]
        return (act[0]["t"] - sp[0]["t"]) / 1000.0 if sp and act else None

    def run_profile(name, extra, tc):
        path = os.path.join(args.profiles, name + ".json")
        booms, count, delay_on, delay_off = profile_sections(path)
        label = name + ("" if not extra else " " + extra[-1])
        print("----- %s: %d boom(s), %d sections, delay %.2f/%.2f s" % (label, booms, count, delay_on, delay_off), flush=True)
        imp = Implement(args.implement, path, iface=args.iface, extra=extra)
        try:
            t = wait(imp, 45.0, lambda: imp.connected)
            check(t is not None, "%s connects (%s)" % (label, "%.1f s" % t if t else "never"))
            if not imp.connected:
                note("  implement stderr: " + imp.err.decode(errors="replace")[-400:])
                return
            lines = tc.new_lines()
            for line in lines:
                if "registered successfully" in line or "Section Control via" in line:
                    note("  TC: " + line.strip())
            check(any(("with %d sections" % count) in l for l in lines), "TC registered %d sections" % count)
            mapped = [l for l in lines if "Mapped DDI" in l]
            with open(path) as f:
                numbers = json.load(f).get("elementNumbers")
            boom_element = numbers[2] if numbers else 2  # device, connector, then the first boom
            odd = [l for l in mapped if ("to element %d" % boom_element) != l.split(")")[-1].strip() and any(("DDI %d " % d) in l for d in range(161, 177))]
            check(not odd, "TC maps the condensed states to the boom element (%d) only" % boom_element + ("" if not odd else ": " + odd[0].strip()))

            # Alternate sections on; the actual follows after the on delay, from the implement's own clock
            pattern = sum(1 << i for i in range(0, count, 2))
            app.sections = pattern
            t = wait(imp, delay_on + 4.0, lambda: imp.actual() == bits(pattern, count))
            check(t is not None, "sections follow the setpoint %s (%s)" % (bits(pattern, count), "%.2f s" % t if t else imp.actual()))
            dt = delay_between(imp, bits(pattern, count), bits(pattern, count))
            check(dt is not None and delay_on - 0.03 <= dt <= delay_on + 0.07, "on delay %s (profile %.2f)" % ("%.3f s" % dt if dt else "?", delay_on))
            t = wait(imp, 3.0, lambda: app.status is not None and app.status[1] == count and bits(app.status[2], count) == bits(pattern, count))
            check(t is not None, "TC PGN 240 reports the implement's actual states (%s)" % (app.status,))

            # The operator: a switch off wins over the TC in auto; master off drops everything
            e = imp.send("switch 1 off")
            t = wait(imp, delay_off + 2.0, lambda: imp.actual() == "0" + bits(pattern, count)[1:])
            check(e and e["ok"] and t is not None, "operator switch 1 off wins over the TC's setpoint (%s)" % imp.actual())
            imp.send("switch 1 on")
            wait(imp, delay_on + 2.0, lambda: imp.actual() == bits(pattern, count))
            imp.send("master off")
            t = wait(imp, delay_off + 2.0, lambda: imp.actual() == "0" * count)
            check(t is not None, "master off: all off")
            imp.send("master on")
            t = wait(imp, delay_on + 2.0, lambda: imp.actual() == bits(pattern, count))
            check(t is not None, "master on: back to the TC's setpoint")

            # All on, all off: the off delay
            app.sections = (1 << count) - 1
            wait(imp, delay_on + 3.0, lambda: imp.actual() == "1" * count)
            check(imp.actual() == "1" * count, "all on: %s" % imp.actual())
            t0 = imp.events[-1]["t"]
            app.sections = 0
            t = wait(imp, delay_off + 4.0, lambda: imp.actual() == "0" * count)
            check(t is not None, "all off (%s)" % ("%.2f s" % t if t else imp.actual()))
            dt = delay_between(imp, "0" * count, "0" * count, after=t0)
            check(dt is not None and delay_off - 0.03 <= dt <= delay_off + 0.07, "off delay %s (profile %.2f)" % ("%.3f s" % dt if dt else "?", delay_off))

            # Section control off at the app: manual, where the operator's switches rule (default on)
            app.sections = pattern
            wait(imp, delay_on + 2.0, lambda: imp.actual() == bits(pattern, count))
            n = len(imp.events)
            app.control = 0
            t = wait(imp, 4.0, lambda: any(not e["auto"] for e in imp.find("mode", since=n)))
            check(t is not None, "section control off: DDI 160 = 0, the implement goes to manual")
            wait(imp, delay_on + 1.0, lambda: False)
            note("  in manual the sections are the operator's switches: %s" % imp.actual())
            app.control = 1
            t = wait(imp, delay_on + 4.0, lambda: imp.actual() == bits(pattern, count))
            check(t is not None, "section control on: auto again, sections back to the setpoint")

            # Speed from the TC's tractor ECU, and the status command agrees
            sp = [e for e in imp.find("speed") if e.get("known")]
            check(sp and abs(sp[-1]["kmh"] - 10.0) < 0.2, "speed from the bus: %s" % (sp[-1]["kmh"] if sp else "none"))
            s = imp.status()
            check(s and s["tc"]["connected"] and s["speed"]["source"] == "bus" and abs(s["speed"]["kmh"] - 10.0) < 0.2, "status: connected, speed from the bus")
            check(s and s["booms"][0]["areaM2"] > 0, "status: area accrued (%.0f m2)" % (s["booms"][0]["areaM2"] if s else 0))
        finally:
            app.sections = 0
            imp.stop()
            if imp.proc.returncode not in (0, -2, None):
                note("  implement exit %s: %s" % (imp.proc.returncode, imp.err.decode(errors="replace")[-300:]))
        t = wait(None, 20.0, lambda: app.status is not None and app.status[1] == 0)
        check(t is not None, "TC drops the implement (%s)" % ("%.1f s" % t if t else "never"))
        late = [l for l in tc.new_lines() if "[Timing]" in l]
        if late:
            note("  TC loop late %d times" % len(late))

    def record_profile(name, extra, tc):
        path = os.path.join(args.profiles, name + ".json")
        booms, count, delay_on, _ = profile_sections(path)
        print("----- %s (recorded): %d boom(s), %d sections" % (name, booms, count), flush=True)
        imp = Implement(args.implement, path, iface=args.iface, extra=extra)
        try:
            t = wait(imp, 45.0, lambda: imp.connected)
            note("%s: connected=%s after %s" % (name, imp.connected, "%.1f s" % t if t else "45 s"))
            tc_log_notes(tc, ("registered", "Section Control via", "boom", "WARNING", "Error", "abort"))
            if imp.connected:
                app.sections = (1 << count) - 1
                n = len(imp.events)
                wait(imp, delay_on + 3.0, lambda: False)
                note("  implement actual: %s" % " | ".join(imp.actual(b) or "-" for b in range(booms)))
                note("  TC PGN 240: %s" % (app.status,))
                sets = imp.find("set", since=n)
                note("  TC sent: %s" % ", ".join("el%s %s=%s" % (e["element"], e.get("what", e["ddi"]), e["value"]) for e in sets[:10]))
        finally:
            app.sections = 0
            imp.stop()
        wait(None, 20.0, lambda: app.status is not None and app.status[1] == 0)

    plan = PLAN if not args.only else [p for p in PLAN if p[0] in args.only]
    recorded = RECORDED if not args.only else [p for p in RECORDED if p[0] in args.only]
    try:
        for name, extra in plan:
            tc = tc_start()
            try:
                run_profile(name, extra, tc)
            finally:
                tc.stop()
        if recorded or not args.only:
            tc = tc_start()
            try:
                for name, extra in recorded:
                    record_profile(name, extra, tc)
                # Back to back on one task controller: a second client at the same address, after the
                # odd ones. Recorded, not checked: AOG-TaskController keeps state across clients.
                if not args.only:
                    print("----- planter-12 after the others on the same task controller (recorded)", flush=True)
                    record_profile("planter-12", [], tc)
            finally:
                tc.stop()
    finally:
        if args.keep_log:
            with open(args.keep_log, "w") as out:
                for path in logs:
                    out.write("===== %s\n" % path)
                    with open(path) as f:
                        out.write(f.read())
    return summary()


if __name__ == "__main__":
    sys.exit(main())
