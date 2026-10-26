#!/usr/bin/env python3
"""Phase 3 (docs/PLAN.md): the implement's screens on a virtual terminal. tools/vt_stub plays a
headless terminal on vcan0; the implement uploads the stack's seeder pool, a key pressed on the
terminal arrives as an event and switches a section, an alarm shows and is acknowledged. Then
the spreader's own pool (pools/spreader.xml): its soft keys drive the master, the rate and the
pages, and its numbers follow the model.

    sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
    tests/scenario_vt.py [build-linux/implement] [build-linux/vt_stub] [vcan0]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from harness import Implement, VtStub, check, note, summary  # noqa: E402

# pools/base_pool_ids.hpp
MAIN_MASK, SETTINGS_MASK, ALARMS_MASK = 1000, 1002, 1003
NO_SPEED_MASK, NO_TC_MASK = 2000, 2001
SECTION1_BUTTON, AUTO_MANUAL_BUTTON = 6000, 6006
SETTINGS_KEY, HOME_KEY = 5002, 5000
STATISTICS_SELECTION = 21001

# pools/spreader_ids.hpp
SP_RUN_MASK, SP_TOTALS_MASK, SP_SPEED_ALARM = 1000, 1001, 2000
SP_MASTER_KEY, SP_RATE_PLUS_KEY, SP_RATE_RESET_KEY, SP_TOTALS_KEY, SP_RUN_KEY = 5000, 5002, 5004, 5005, 5006
SP_TARGET_RATE, SP_GROUND_SPEED, SP_BIN_LEVEL, SP_MASTER_STATUS = 12000, 12002, 12003, 27001
SP_MASTER_ON, SP_MASTER_OFF = 11013, 11014


def main():
    implement = sys.argv[1] if len(sys.argv) > 1 else "build-linux/implement"
    stub = sys.argv[2] if len(sys.argv) > 2 else "build-linux/vt_stub"
    iface = sys.argv[3] if len(sys.argv) > 3 else "vcan0"
    profiles = "profiles"

    vt = VtStub(stub, iface)
    imp = None
    try:
        t = wait(vt, None, 10.0, lambda: vt.find("ready"))
        check(t is not None, "vt_stub up on %s" % iface)

        imp = Implement(implement, os.path.join(profiles, "planter-12.json"), iface=iface)
        t = wait(vt, imp, 40.0, lambda: vt.pool == "loaded")
        check(t is not None, "the pool reaches the terminal and parses (%s)" % ("%.1f s" % t if t else vt.pool))
        t = wait(vt, imp, 10.0, lambda: imp.find("vt", connected=True))
        check(t is not None, "the implement reports the terminal connected")
        t = wait(vt, imp, 5.0, lambda: vt.mask == MAIN_MASK)
        check(t is not None, "the run screen is the active mask (%s)" % vt.mask)

        # A button on the terminal switches a section: manual mode first, so switches show
        imp.send("auto off")
        t = wait(vt, imp, 2.0, lambda: imp.actual() == "1" * 12)
        check(t is not None, "manual: all sections on (%s)" % imp.actual())
        n = len(imp.events)
        vt.send("button %d" % SECTION1_BUTTON)
        t = wait(vt, imp, 3.0, lambda: imp.find("key", since=n, id=SECTION1_BUTTON, code="released"))
        check(t is not None, "the section 1 button press arrives as a key event")
        t = wait(vt, imp, 2.0, lambda: imp.actual() == "0" + "1" * 11)
        check(t is not None, "and switches section 1 off (%s)" % imp.actual())
        vt.send("button %d" % AUTO_MANUAL_BUTTON)
        t = wait(vt, imp, 3.0, lambda: imp.find("mode", since=n, auto=True))
        check(t is not None, "the auto/manual button puts the implement in auto")
        imp.send("auto off")

        # A soft key changes the mask; the terminal sees it
        vt.send("key %d" % SETTINGS_KEY)
        t = wait(vt, imp, 3.0, lambda: vt.mask == SETTINGS_MASK)
        check(t is not None, "the settings soft key shows the settings mask (%s)" % vt.mask)
        e = imp.send("mask %d" % MAIN_MASK)
        t = wait(vt, imp, 3.0, lambda: vt.mask == MAIN_MASK)
        check(e and e["ok"] and t is not None, "mask command: back to the run screen")

        # No speed for 10 s: the alarm raises on its own; the terminal acknowledges it
        t = wait(vt, imp, 15.0, lambda: vt.mask == NO_SPEED_MASK)
        check(t is not None, "no-speed alarm mask shows on its own (%s)" % vt.mask)
        raised = imp.find("alarm", name="noSpeed", state="raised")
        check(bool(raised), "alarm event raised")
        vt.send("ack")
        t = wait(vt, imp, 3.0, lambda: imp.find("alarm", name="noSpeed", state="acknowledged"))
        check(t is not None, "acknowledged from the terminal")
        t = wait(vt, imp, 3.0, lambda: vt.mask == MAIN_MASK)
        check(t is not None, "back to the run screen after the acknowledge (%s)" % vt.mask)
        if t is None:
            note("  implement mask/alarm/vt events: %s" % ", ".join(
                "%s@%d:%s" % (e["event"], e["t"], e.get("mask", e.get("state", e.get("connected")))) for e in imp.events[-40:] if e["event"] in ("mask", "alarm", "vt")))
            note("  stub mask events: %s" % ", ".join("%d@%d" % (e["mask"], e["t"]) for e in vt.find("mask")[-8:]))
            err = imp.err.decode(errors="replace")
            note("  implement stderr: %d lines, %d timeouts; tail: %s" % (err.count("\n"), err.count("timed out"), err[-300:].replace("\n", " | ")))
        imp.send("speed 10")
        t = wait(vt, imp, 3.0, lambda: imp.find("alarm", name="noSpeed", state="cleared"))
        check(t is not None, "speed back: alarm cleared")

        # Forced alarm by command, cleared by command
        e = imp.send("alarm noSpeed")
        t = wait(vt, imp, 3.0, lambda: vt.mask == NO_SPEED_MASK)
        check(e and e["ok"] and t is not None, "alarm noSpeed command shows the mask")
        e = imp.send("alarm clear")
        t = wait(vt, imp, 3.0, lambda: vt.mask == MAIN_MASK)
        check(e and e["ok"] and t is not None, "alarm clear: run screen again")
        e = imp.send("alarm bogus")
        check(e and not e["ok"], "an unknown alarm is refused: %s" % (e and e.get("error")))

        # A value entered on the terminal, and one sent to it
        n = len(imp.events)
        vt.send("value %d 2" % STATISTICS_SELECTION)
        t = wait(vt, imp, 3.0, lambda: imp.find("value", since=n, id=STATISTICS_SELECTION, value=2))
        check(t is not None, "a value entered on the terminal arrives as an event")
        e = imp.send("value %d 1" % STATISTICS_SELECTION)
        check(e and e["ok"], "value command accepted")
        e = imp.send("key %d" % HOME_KEY)
        check(e and e["ok"] and imp.find("key", id=HOME_KEY, source="command"), "key command acts as a press")

        # The no-task-controller alarm after its 30 s
        t = wait(vt, imp, 40.0, lambda: imp.find("alarm", name="noTaskController", state="raised"))
        check(t is not None, "no-task-controller alarm after 30 s")
        s = imp.status()
        check(s and s["vt"]["connected"] and s["vt"]["mask"] == NO_TC_MASK, "status shows the terminal and its mask: %s" % (s and s.get("vt")))

        # Speed on the screen: the readout value sent to the terminal
        vals = vt.find("change", id=21012)
        check(vals and vals[-1]["value"] == 2778, "speed readout sent to the terminal: %s" % (vals[-1]["value"] if vals else None))
        err = imp.err.decode(errors="replace")
        note("implement stderr: %d lines, %d command timeouts" % (err.count("\n"), err.count("timed out")))
    finally:
        if imp:
            imp.stop()
            if imp.proc.returncode not in (0, -2, None):
                note("implement stderr: " + imp.err.decode(errors="replace")[-400:])
        vt.stop()
        if vt.proc.returncode not in (0, None):
            note("vt_stub stderr: " + vt.err.decode(errors="replace")[-400:])
    spreader(implement, stub, iface, profiles)
    return summary()


def spreader(implement, stub, iface, profiles):
    vt = VtStub(stub, iface)
    imp = None
    try:
        wait(vt, None, 10.0, lambda: vt.find("ready"))
        imp = Implement(implement, os.path.join(profiles, "spreader-9m.json"), iface=iface)
        t = wait(vt, imp, 40.0, lambda: vt.pool == "loaded")
        check(t is not None, "spreader: its pool reaches the terminal and parses (%s)" % ("%.1f s" % t if t else vt.pool))
        t = wait(vt, imp, 5.0, lambda: vt.mask == SP_RUN_MASK)
        check(t is not None, "spreader: the run screen is the active mask (%s)" % vt.mask)
        t = wait(vt, imp, 3.0, lambda: any(c["value"] == 2000 for c in vt.find("change", id=SP_TARGET_RATE)))
        check(t is not None, "spreader: the target rate on screen is the profile's 2000 kg/ha")
        t = wait(vt, imp, 3.0, lambda: any(c["value"] == 8000 for c in vt.find("change", id=SP_BIN_LEVEL)))
        check(t is not None, "spreader: the bin shows 8000 kg")

        imp.send("speed 10")
        t = wait(vt, imp, 3.0, lambda: any(c["value"] == 2778 for c in vt.find("change", id=SP_GROUND_SPEED)))
        check(t is not None, "spreader: 10 km/h reaches the speed readout as 2778 mm/s")

        vt.send("key %d" % SP_MASTER_KEY)
        t = wait(vt, imp, 3.0, lambda: (imp.status() or {}).get("booms", [{}])[0].get("master") is False)
        check(t is not None, "spreader: the master key turns the master off")
        t = wait(vt, imp, 3.0, lambda: any(c["value"] == SP_MASTER_OFF for c in vt.find("change", id=SP_MASTER_STATUS)))
        check(t is not None, "spreader: and the screen says MASTER OFF")
        vt.send("key %d" % SP_MASTER_KEY)
        t = wait(vt, imp, 3.0, lambda: (imp.status() or {}).get("booms", [{}])[0].get("master") is True)
        check(t is not None, "spreader: pressed again, the master is back on")

        vt.send("key %d" % SP_RATE_PLUS_KEY)
        t = wait(vt, imp, 3.0, lambda: abs(((imp.status() or {}).get("products", [{}])[0].get("target") or 0) - 2200) < 1)
        check(t is not None, "spreader: rate + steps the operator's rate to 2200 kg/ha")
        vt.send("key %d" % SP_RATE_RESET_KEY)
        t = wait(vt, imp, 3.0, lambda: abs(((imp.status() or {}).get("products", [{}])[0].get("target") or 0) - 2000) < 1)
        check(t is not None, "spreader: rate 100% returns to 2000 kg/ha")

        vt.send("key %d" % SP_TOTALS_KEY)
        t = wait(vt, imp, 3.0, lambda: vt.mask == SP_TOTALS_MASK)
        check(t is not None, "spreader: the totals key shows the totals screen (%s)" % vt.mask)
        vt.send("key %d" % SP_RUN_KEY)
        t = wait(vt, imp, 3.0, lambda: vt.mask == SP_RUN_MASK)
        check(t is not None, "spreader: the run key goes back (%s)" % vt.mask)

        e = imp.send("alarm noSpeed")
        t = wait(vt, imp, 3.0, lambda: vt.mask == SP_SPEED_ALARM)
        check(e and e["ok"] and t is not None, "spreader: the no-speed alarm shows its own mask (%s)" % vt.mask)
        vt.send("ack")
        t = wait(vt, imp, 3.0, lambda: vt.mask == SP_RUN_MASK)
        check(t is not None, "spreader: acknowledged, back to the run screen (%s)" % vt.mask)
        bad = [e for e in imp.events if e["event"] == "mask" and e.get("errors")]
        check(not bad, "spreader: the terminal reported no object errors (%s)" % bad[:2])
    finally:
        if imp:
            imp.stop()
        vt.stop()


def wait(vt, imp, seconds, cond):
    start = time.time()
    while time.time() - start < seconds:
        vt.poll()
        if imp:
            imp.poll()
        if cond():
            return time.time() - start
        time.sleep(0.01)
    return None


if __name__ == "__main__":
    sys.exit(main())
