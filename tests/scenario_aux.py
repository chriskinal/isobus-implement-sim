#!/usr/bin/env python3
"""Phase 7 (docs/PLAN.md): auxiliary functions and inputs. The implement with the auxiliary
functions pool and the simulated joystick both reach the headless terminal; the joystick's
button and slider are accepted. The assignment itself is the terminal's job and vt_stub does
not do AUX-N, so "an assignment made on a terminal is stored and used" is checked by hand on
AgIsoVirtualTerminal; what can be checked here is checked.

    tests/scenario_aux.py [build-linux/implement] [build-linux/vt_stub] [build-linux/aux_input] [vcan0]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from harness import AuxInput, Implement, VtStub, check, note, summary  # noqa: E402


def main():
    implement = sys.argv[1] if len(sys.argv) > 1 else "build-linux/implement"
    stub = sys.argv[2] if len(sys.argv) > 2 else "build-linux/vt_stub"
    joystick = sys.argv[3] if len(sys.argv) > 3 else "build-linux/aux_input"
    iface = sys.argv[4] if len(sys.argv) > 4 else "vcan0"

    vt = VtStub(stub, iface)
    imp = aux = None

    def wait(seconds, cond):
        start = time.time()
        while time.time() - start < seconds:
            vt.poll()
            if imp:
                imp.poll()
            if aux:
                aux.poll()
            if cond():
                return time.time() - start
            time.sleep(0.01)
        return None

    try:
        check(wait(10.0, lambda: vt.find("ready")) is not None, "vt_stub up")
        imp = Implement(implement, "profiles/sprayer-24.json", iface=iface, extra=["--pool", "pools/aux_functions.iop"])
        t = wait(40.0, lambda: imp.find("vt", connected=True))
        check(t is not None, "the implement with the auxiliary functions pool connects to the terminal (%s)" % ("%.1f s" % t if t else "never"))
        e = imp.find("vt", pool=True)
        check(e and e[0].get("auxFunctions") is True, "the pool is recognised as the auxiliary functions pool")
        loaded = [p for p in vt.find("pool") if p["state"] == "loaded"]
        check(len(loaded) >= 1, "the terminal parsed the pool")

        aux = AuxInput(joystick, iface)
        t = wait(40.0, lambda: aux.connected)
        check(t is not None, "the joystick connects to the terminal (%s)" % ("%.1f s" % t if t else "never"))
        loaded = [p for p in vt.find("pool") if p["state"] == "loaded"]
        check(len(loaded) >= 2, "the terminal parsed both pools (%d)" % len(loaded))
        aux.send("button on")
        t = wait(3.0, lambda: aux.find("command", command="button on"))
        e = aux.find("command", command="button on")
        check(e and e[0]["ok"] and e[0]["button"] is True and e[0]["transitions"] == 1, "button on: state and transition count")
        aux.send("slider 32000")
        t = wait(3.0, lambda: aux.find("command", command="slider 32000"))
        e = aux.find("command", command="slider 32000")
        check(e and e[0]["ok"] and e[0]["slider"] == 32000, "slider position taken")
        note("assignment of inputs to functions needs a terminal that does AUX-N (AgIsoVirtualTerminal): by hand")
        note("implement aux events so far: %d; joystick learn-mode events: %d" % (len(imp.find("aux")), len(aux.find("learn"))))
        s = imp.status()
        check(s and s["vt"]["connected"], "status: terminal connected with the aux pool: %s" % (s and s["vt"]["pool"]))
    finally:
        if aux:
            aux.stop()
        if imp:
            imp.stop()
        vt.stop()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
