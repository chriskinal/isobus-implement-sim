#!/usr/bin/env python3
"""Commands and events without a task controller (docs/PLAN.md phase 2): the implement on the
stack's virtual driver, driven over its TCP port. Runs anywhere the binary builds.

    tests/scenario_commands.py [build/implement] [profiles]
"""
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
from harness import Implement, check, note, summary  # noqa: E402


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/implement"
    profiles = sys.argv[2] if len(sys.argv) > 2 else "profiles"

    imp = Implement(binary, os.path.join(profiles, "sprayer-24.json"), driver="virtual")
    try:
        check(imp.port and imp.port > 0, "the implement listens on a port (%s) and reports it" % imp.port)
        e = imp.send("help")
        check(e and e["ok"] and "switch" in e["help"], "help answers over TCP")
        e = imp.send("bogus 1 2")
        check(e and not e["ok"], "an unknown command is refused: %s" % (e and e.get("error", "")[:40]))

        s = imp.status()
        check(s and s["type"] == "sprayer" and len(s["booms"]) == 1 and s["booms"][0]["actual"] == "0" * 12, "status: 12 sections off")
        check(s and s["speed"]["known"] is False and s["tc"]["connected"] is False, "status: no speed, no task controller")

        # Manual mode: the operator's switches and master drive the sections, with the profile's delays
        imp.send("auto off")
        imp.send("switch 3 off")
        t = imp.wait(lambda i: i.actual() == "110111111111", 2.0)
        check(t is not None and 0.25 <= t <= 0.45, "manual: switches rule, section 3 off after the on delay (%s)" % ("%.2f s" % t if t else imp.actual()))
        e = imp.send("switch 13 on")
        check(e and not e["ok"], "switch 13 refused on a 12-section machine")
        imp.send("master off")
        t = imp.wait(lambda i: i.actual() == "0" * 12, 2.0)
        check(t is not None and 0.15 <= t <= 0.35, "master off: all off after the off delay (%s)" % ("%.2f s" % t if t else imp.actual()))
        imp.send("master on")
        imp.wait(lambda i: i.actual() == "110111111111", 2.0)
        imp.send("switch 3 on")
        imp.wait(lambda i: i.actual() == "1" * 12, 2.0)
        check(imp.actual() == "1" * 12, "all on again")
        mode = imp.find("mode")
        check(mode and mode[-1]["master"] is True and mode[-1]["auto"] is False, "mode events report manual and master")

        # Speed from the command; rate, flow and pressure follow (SPRAYER.md section 5)
        e = imp.send("speed 12")
        check(e and e["ok"] and e["source"] == "fixed", "speed 12 km/h fixed")
        t = imp.wait(lambda i: any(ev.get("known") and abs(ev["kmh"] - 12.0) < 0.1 for ev in i.find("speed")), 2.0)
        check(t is not None, "speed event shows 12.0 km/h")
        imp.wait(lambda i: False, 0.3)
        s = imp.status()
        p = s["products"][0]
        check(abs(p["actual"] - 150.0) < 0.1, "actual rate 150 l/ha while spraying at speed (%.1f)" % p["actual"])
        check(abs(p["flowLPerMin"] - 72.0) < 0.5, "flow 72 l/min (%.1f)" % p["flowLPerMin"])
        check(abs(p["pressureBar"] - 2.64) < 0.05, "pressure 2.64 bar (%.2f)" % p["pressureBar"])
        check(p["level"] < 3000.0, "the tank drains (%.3f l)" % p["level"])

        # The operator's rate applies only with prescription control off (the TC's state, default on)
        e = imp.send("rate 200")
        check(e and e["ok"] and e["inEffect"] is False, "rate 200 stored, not in effect while prescription control is on")
        e = imp.send("tank 100")
        check(e and e["ok"] and abs(e["level"] - 100) < 0.01, "tank set to 100 l")
        e = imp.send("fill")
        check(e and e["ok"] and abs(e["level"] - 3000) < 0.01, "fill: 3000 l")
        e = imp.send("speed bus")
        check(e and e["ok"], "speed back to the bus")
        t = imp.wait(lambda i: i.find("speed") and i.find("speed")[-1]["known"] is False, 3.0)
        check(t is not None, "speed unknown without a tractor ECU")
        s = imp.status()
        check(s["products"][0]["actual"] == 0.0, "no rate without speed")

        # A blocked nozzle reports error and drops out
        e = imp.send("fault nozzle 5 blocked")
        t = imp.wait(lambda i: i.actual() == "1111E1111111", 2.0)
        check(t is not None, "blocked nozzle 5 shows E (%s)" % imp.actual())
        e = imp.send("fault nozzle 5 clear")
        t = imp.wait(lambda i: i.actual() == "1" * 12, 2.0)
        check(t is not None, "cleared")
        e = imp.send("fault bogus")
        check(e and not e["ok"], "an unknown fault is refused")
        e = imp.send("fault slow 10")
        check(e and e["ok"] and e["ms"] == 10, "fault slow 10 on the virtual driver")
        imp.send("fault slow 0")

        # quit ends the process cleanly
        imp.send("quit", timeout=0.5)
        t = imp.wait(lambda i: i.proc.poll() is not None, 5.0)
        check(t is not None and imp.proc.returncode == 0, "quit: exit %s" % imp.proc.returncode)
        check(any(e["event"] == "exit" for e in imp.events), "exit event on stdout")
    finally:
        imp.stop()

    # A seeder: depth, bin, a blocked row, two products on the drill
    imp = Implement(binary, os.path.join(profiles, "drill-6m.json"), driver="virtual")
    try:
        e = imp.send("depth 40")
        check(e and not e["ok"], "depth refused on a drill (planter only)")
        e = imp.send("bin 300 Fertilizer")
        check(e and e["ok"] and e["product"] == "Fertilizer" and e["level"] == 300, "bin by product name")
        e = imp.send("bin 50 2")
        check(e and e["ok"] and e["product"] == "Fertilizer", "bin by product index")
        e = imp.send("fault row 2 blocked")
        t = imp.wait(lambda i: i.actual() == "0E00", 2.0)
        check(t is not None, "blocked row 2 on the drill: %s" % imp.actual())
        s = imp.status()
        check(len(s["products"]) == 2 and s["products"][1]["level"] == 50, "status lists both products")
    finally:
        imp.stop()

    imp = Implement(binary, os.path.join(profiles, "planter-12.json"), driver="virtual")
    try:
        e = imp.send("depth 40")
        check(e and e["ok"] and e["depthMm"] == 40, "planter depth 40 mm")
        e = imp.send("depth 500")
        check(e and e["ok"] and e["depthMm"] == 90, "depth clipped to the profile's maximum")
    finally:
        imp.stop()

    # Two clients see the same events; stdin takes commands too
    imp = Implement(binary, os.path.join(profiles, "sprayer-mounted-12.json"), driver="virtual")
    try:
        import socket
        other = socket.create_connection(("127.0.0.1", imp.port))
        other.settimeout(3.0)
        imp.send("switch 2 off")
        data = b""
        deadline = __import__("time").time() + 3
        while b'"switch 2 off"' not in data and __import__("time").time() < deadline:
            try:
                data += other.recv(65536)
            except socket.timeout:
                break
        check(b'"switch 2 off"' in data and b'"event":"client"' in data, "a second client gets the events")
        other.close()
        imp.proc.stdin = None
    finally:
        imp.stop()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
