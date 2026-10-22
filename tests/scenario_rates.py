#!/usr/bin/env python3
"""Phase 5 (docs/PLAN.md): rates and the rest of the process data, against tools/tc_stub, a
headless task controller that sends rates. A rate setpoint from the task controller is
followed with the profile's time constant; totals add up over a simulated hectare; the bin
drains; a rate outside the limits is clipped; prescription control off hands over to the
operator's rate.

    tests/scenario_rates.py [build-linux/implement] [build-linux/tc_stub] [vcan0]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from harness import Implement, TcStub, check, note, summary  # noqa: E402

# DDIs (docs/MODEL.md sections 3 to 5)
SETPOINT_VOLUME_RATE, ACTUAL_VOLUME_RATE = 1, 2
ACTUAL_VOLUME_CONTENT = 72
APPLICATION_TOTAL_VOLUME = 80
TOTAL_AREA, EFFECTIVE_TIME = 116, 119
ACTUAL_WORK_STATE, PRESCRIPTION_CONTROL = 141, 158
SECTION_CONTROL_STATE = 160
SETPOINT_WORK_STATE, SETPOINT_CONDENSED = 289, 290
ACTUAL_FLOW, ACTUAL_PRESSURE = 37, 194
L_PER_HA = 10000  # DDI 1 and 2 are 0.01 mm3/m2


def main():
    implement = sys.argv[1] if len(sys.argv) > 1 else "build-linux/implement"
    stub = sys.argv[2] if len(sys.argv) > 2 else "build-linux/tc_stub"
    iface = sys.argv[3] if len(sys.argv) > 3 else "vcan0"

    tc = TcStub(stub, iface)
    imp = None

    def wait(seconds, cond):
        start = time.time()
        while time.time() - start < seconds:
            tc.poll()
            if imp:
                imp.poll()
            if cond():
                return time.time() - start
            time.sleep(0.01)
        return None

    try:
        t = wait(10.0, lambda: tc.find("ready"))
        check(t is not None, "tc_stub up on %s" % iface)
        imp = Implement(implement, "profiles/sprayer-24.json", iface=iface)
        t = wait(45.0, lambda: imp.connected and tc.client)
        check(t is not None, "the sprayer connects to the stub task controller (%s)" % ("%.1f s" % t if t else "never"))
        check(tc.client and tc.client["device"] == "Sprayer 24 m" and tc.client["sections"] == 12, "the stub parsed the DDOP: %s, %s sections" % (tc.client and tc.client["device"], tc.client and tc.client["sections"]))
        bin_el = tc.element_for(SETPOINT_VOLUME_RATE)
        boom_el = tc.element_for(SETPOINT_CONDENSED)
        check(bin_el is not None and boom_el is not None, "rate on element %s, sections on element %s" % (bin_el, boom_el))

        # Measurements: the actual rate every 500 ms, the content every second, the area as it changes
        tc.send("interval %d %d 500" % (ACTUAL_VOLUME_RATE, bin_el))
        tc.send("interval %d %d 1000" % (ACTUAL_VOLUME_CONTENT, bin_el))
        tc.send("interval %d %d 1000" % (TOTAL_AREA, boom_el))
        tc.send("interval %d %d 1000" % (APPLICATION_TOTAL_VOLUME, bin_el))

        # All sections on at 12 km/h: the actual rises to 150 l/ha with the 1 s time constant
        imp.send("speed 12")
        tc.send("set %d %d 1" % (SETPOINT_WORK_STATE, boom_el))
        tc.send("set %d %d %d" % (SETPOINT_CONDENSED, boom_el, 0x555555))  # 12 sections on
        t = wait(3.0, lambda: imp.actual() == "1" * 12)
        check(t is not None, "all sections on (%s)" % imp.actual())
        t0 = time.time()
        t = wait(6.0, lambda: tc.values.get((ACTUAL_VOLUME_RATE, bin_el), 0) >= 147 * L_PER_HA)
        check(t is not None and t <= 5.5, "actual rate reaches 147 l/ha within five time constants (%s)" % ("%.1f s" % t if t else tc.values.get((ACTUAL_VOLUME_RATE, bin_el))))

        # A setpoint from the task controller: 200 l/ha, followed; the implement acknowledges with its own event
        n = len(imp.events)
        tc.send("set %d %d %d" % (SETPOINT_VOLUME_RATE, bin_el, 200 * L_PER_HA))
        t = wait(3.0, lambda: imp.find("set", since=n, ddi=SETPOINT_VOLUME_RATE))
        check(t is not None, "the rate setpoint arrives as a set event")
        e = imp.find("set", since=n, ddi=SETPOINT_VOLUME_RATE)
        check(e and abs(e[0]["rate"] - 200.0) < 0.01 and not e[0]["clipped"], "200 l/ha, not clipped: %s" % (e and e[0]))
        t = wait(8.0, lambda: tc.values.get((ACTUAL_VOLUME_RATE, bin_el), 0) >= 196 * L_PER_HA)
        check(t is not None, "the actual follows to 196 l/ha and above (%s)" % ("%.1f s" % t if t else tc.values.get((ACTUAL_VOLUME_RATE, bin_el))))
        wait(3.0, lambda: False)  # settled
        s = imp.status()
        check(s and abs(s["products"][0]["pressureBar"] - 4.69) < 0.4, "pressure near 4.7 bar at 200 l/ha (%.2f)" % s["products"][0]["pressureBar"])

        # Outside the limits: clipped to the maximum, and reported so
        n = len(imp.events)
        tc.send("set %d %d %d" % (SETPOINT_VOLUME_RATE, bin_el, 900 * L_PER_HA))
        t = wait(3.0, lambda: imp.find("set", since=n, ddi=SETPOINT_VOLUME_RATE))
        e = imp.find("set", since=n, ddi=SETPOINT_VOLUME_RATE)
        check(e and e[0]["clipped"], "900 l/ha is clipped (profile maximum 400)")
        s = imp.status()
        check(s and abs(s["products"][0]["target"] - 400.0) < 0.01, "target is the maximum, 400 (%.0f)" % s["products"][0]["target"])

        # Prescription control off: the operator's rate rules
        imp.send("rate 120")
        tc.send("set %d %d 0" % (PRESCRIPTION_CONTROL, bin_el))
        t = wait(3.0, lambda: (s2 := imp.status()) and abs(s2["products"][0]["target"] - 120.0) < 0.01)
        check(t is not None, "prescription control off: the operator's 120 l/ha is the target")
        tc.send("set %d %d 1" % (PRESCRIPTION_CONTROL, bin_el))
        t = wait(3.0, lambda: (s2 := imp.status()) and abs(s2["products"][0]["target"] - 400.0) < 0.01)
        check(t is not None, "prescription control on: back to the task controller's setpoint")

        # One simulated hectare at 150 l/ha: 24 m at 12 km/h is 417 m in 125 s. Reset the totals first.
        tc.send("set %d %d %d" % (SETPOINT_VOLUME_RATE, bin_el, 150 * L_PER_HA))
        imp.send("fill")
        imp.send("speed 0")  # standing still while the totals are reset
        wait(2.0, lambda: False)
        tc.send("set %d %d 0" % (TOTAL_AREA, boom_el))
        tc.send("set %d %d 0" % (APPLICATION_TOTAL_VOLUME, bin_el))
        wait(1.5, lambda: False)
        s0 = imp.status()
        area0, trip0, level0 = s0["booms"][0]["areaM2"], s0["products"][0]["tripTotal"], s0["products"][0]["level"]
        check(area0 < 1 and trip0 < 0.1, "totals reset by the task controller (area %.1f m2, trip %.2f l)" % (area0, trip0))
        imp.send("speed 12")
        wait(5.0, lambda: False)  # the rate settles before the hectare is timed
        s0 = imp.status()
        area0, trip0, level0 = s0["booms"][0]["areaM2"], s0["products"][0]["tripTotal"], s0["products"][0]["level"]
        start = time.time()
        wait(125.0, lambda: False)
        s1 = imp.status()
        elapsed = time.time() - start
        note("  before: speed %s sections %s; after: speed %s sections %s rate %.1f" % (
            s0["speed"], s0["booms"][0]["actual"], s1["speed"], s1["booms"][0]["actual"], s1["products"][0]["actual"]))
        area = s1["booms"][0]["areaM2"] - area0
        expected_area = 24.0 * (12.0 / 3.6) * elapsed
        check(abs(area - expected_area) / expected_area < 0.01, "area total %.0f m2 for %.1f s at 12 km/h (expected %.0f)" % (area, elapsed, expected_area))
        volume = s1["products"][0]["tripTotal"] - trip0
        check(abs(volume - 150.0 * expected_area / 10000.0) / (150.0 * expected_area / 10000.0) < 0.03, "volume total %.1f l (expected %.1f)" % (volume, 150.0 * expected_area / 10000.0))
        check(abs((level0 - s1["products"][0]["level"]) - volume) < 1.0, "the tank drained by the same (%.1f l)" % (level0 - s1["products"][0]["level"]))
        # Flow and pressure by request while still spraying
        tc.send("request %d %d" % (ACTUAL_FLOW, bin_el))
        tc.send("request %d %d" % (ACTUAL_PRESSURE, bin_el))
        t = wait(3.0, lambda: (ACTUAL_FLOW, bin_el) in tc.values and (ACTUAL_PRESSURE, bin_el) in tc.values)
        check(t is not None and abs(tc.values[(ACTUAL_FLOW, bin_el)] / 16666.67 - 72.0) < 3.0, "flow by request: %.1f l/min" % (tc.values.get((ACTUAL_FLOW, bin_el), 0) / 16666.67))
        check(t is not None and abs(tc.values[(ACTUAL_PRESSURE, bin_el)] / 100000.0 - 2.64) < 0.2, "pressure by request: %.2f bar" % (tc.values.get((ACTUAL_PRESSURE, bin_el), 0) / 100000.0))

        # Standing still, the task controller's totals and the implement's agree
        imp.send("speed 0")
        wait(2.5, lambda: False)
        tc.send("request %d %d" % (APPLICATION_TOTAL_VOLUME, bin_el))
        tc.send("request %d %d" % (TOTAL_AREA, boom_el))
        wait(2.0, lambda: False)
        s2 = imp.status()
        reported = tc.values.get((APPLICATION_TOTAL_VOLUME, bin_el))
        check(reported is not None and abs(reported - s2["products"][0]["tripTotal"]) <= 2, "the task controller sees the volume total: %s l (implement %.1f)" % (reported, s2["products"][0]["tripTotal"]))
        reported_area = tc.values.get((TOTAL_AREA, boom_el))
        check(reported_area is not None and abs(reported_area - s2["booms"][0]["areaM2"]) <= 30, "and the area total: %s m2 (implement %.0f)" % (reported_area, s2["booms"][0]["areaM2"]))

        # Speed gone: the rate falls to zero and nothing accrues
        imp.send("speed bus")
        t = wait(6.0, lambda: tc.values.get((ACTUAL_VOLUME_RATE, bin_el), 1) == 0)
        check(t is not None, "no speed: the reported rate is zero")
    finally:
        if imp:
            imp.stop()
            if imp.proc.returncode not in (0, -2, None):
                note("implement stderr: " + imp.err.decode(errors="replace")[-400:])
        tc.stop()
        if tc.proc.returncode not in (0, None):
            note("tc_stub stderr: " + tc.err.decode(errors="replace")[-400:])
    return summary()


if __name__ == "__main__":
    sys.exit(main())
