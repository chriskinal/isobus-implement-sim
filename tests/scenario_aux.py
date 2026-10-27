#!/usr/bin/env python3
"""Phase 7 (docs/PLAN.md): auxiliary functions and inputs. The implement with the auxiliary
functions pool and the simulated joystick both reach the headless terminal; the joystick's
button and slider are accepted, and so are the two axes and five buttons of the joystick pool.
vt_stub then makes assignments as an operator would (it has no AUX-N of its own, so it sends the
assignment command) and the joystick drives the spreader's functions through them. The assignment itself is the terminal's job and vt_stub does
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

        # The joystick pool: two axes and five buttons. Same NAME, so the first device goes first.
        aux.stop()
        aux = None
        pools_before = len([p for p in vt.find("pool") if p["state"] == "loaded"])
        aux = AuxInput(joystick, iface, pool="pools/joystick.iop")
        t = wait(40.0, lambda: aux.connected)
        check(t is not None, "the joystick with the joystick pool connects (%s)" % ("%.1f s" % t if t else "never"))
        ready = aux.find("ready")
        names = sorted(ready[0]["inputs"]) if ready else []
        check(names == ["a", "b", "c", "d", "ok", "x", "y"], "the joystick pool's seven inputs: %s" % names)
        t = wait(10.0, lambda: len([p for p in vt.find("pool") if p["state"] == "loaded"]) > pools_before)
        check(t is not None, "the terminal parsed the joystick pool")
        aux.send("axis y 1000")
        wait(3.0, lambda: aux.find("command", command="axis y 1000"))
        e = aux.find("command", command="axis y 1000")
        check(e and e[0]["ok"] and e[0]["inputs"]["y"] == 1000 and e[0]["inputs"]["x"] == 32127, "axis y moves y only, x stays in the middle")
        aux.send("button ok on")
        wait(3.0, lambda: aux.find("command", command="button ok on"))
        e = aux.find("command", command="button ok on")
        check(e and e[0]["ok"] and e[0]["inputs"]["ok"] is True and e[0]["inputs"]["a"] is False, "button ok on presses ok only")
        aux.send("button e on")
        wait(3.0, lambda: aux.find("command", command="button e on"))
        e = aux.find("command", command="button e on")
        check(e and e[0]["ok"] is False, "a button the pool does not have is refused")

        # Assignment, as an operator makes it on a terminal that does AUX-N: vt_stub sends the
        # command to the spreader (its pool has the functions), and the joystick drives them
        imp.stop()
        imp = Implement(implement, "profiles/spreader-9m.json", iface=iface, extra=["--log", "info"])  # the stack's AUX-N lines
        t = wait(40.0, lambda: imp.find("vt", connected=True))
        check(t is not None, "the spreader connects to the terminal")
        aux.send("axis y 32127")  # centred, as a stick at rest is, before the trim is assigned to it
        wait(1.0, lambda: False)  # the joystick's maintenance message, so the spreader knows it

        def product():
            s = imp.status()
            return s["products"][0] if s else {}

        def assign(function, input_id):
            n = len(vt.find("command"))
            vt.send("assign %d %d" % (function, input_id))
            wait(3.0, lambda: len(vt.find("command")) > n)
            e = vt.find("command")[n:]
            wait(0.5, lambda: False)
            return bool(e and e[0]["ok"])

        def press(button, hold=0.3):
            aux.send("button %s on" % button)
            wait(hold, lambda: False)
            aux.send("button %s off" % button)
            wait(0.5, lambda: False)

        check(assign(6012, 3010), "assign RATE+ to button A")
        press("a")
        p = product()
        stepped = abs(p.get("target", 0) - 2200) < 1 and p.get("prescription") is False
        check(stepped, "button A steps the rate up a tenth: %s" % p.get("target"))
        if not stepped:  # once in five runs on the VM, not reproduced: what the stack said
            note("  the spreader's AUX-N log: %s" % [l for l in imp.err.decode(errors="replace").splitlines() if "AUX" in l])
            note("  its aux events: %s" % [(e["function"], e["value1"], e["t"]) for e in imp.find("aux")])
        check(assign(6014, 3013), "assign RESET to button D")
        press("d")
        check(abs(product().get("target", 0) - 2000) < 1, "button D resets the rate: %s" % product().get("target"))

        check(assign(6010, 3014), "assign MASTER to the stick's press")
        press("ok", hold=2.5)  # held past the input's one-second repeat
        s = imp.status()
        check(s and s["booms"][0]["master"] is False, "the press turns the master off once, holding it does not turn it back")
        press("ok")
        s = imp.status()
        check(s and s["booms"][0]["master"] is True, "a second press turns it back on")

        check(assign(6000, 3001), "assign TRIM to the stick's Y axis")
        before = product().get("target", 0)
        aux.send("axis y 64255")
        wait(1.0, lambda: False)
        aux.send("axis y 32127")
        wait(0.5, lambda: False)
        after = product().get("target", 0)
        check(abs(before - 2000) < 1 and 2200 < after < 2900, "a second at full deflection raises the rate about a quarter: %.0f to %.0f" % (before, after))
        wait(1.0, lambda: False)
        check(abs(product().get("target", 0) - after) < 1, "let go, the rate stays")
        check([e["active"] for e in imp.find("trim")][-2:] == [True, False], "the trim starts and stops")

        n = len(vt.find("command"))
        vt.send("unassign 6012")
        wait(3.0, lambda: len(vt.find("command")) > n)
        wait(0.5, lambda: False)
        before = product().get("target", 0)
        press("a")
        check(abs(product().get("target", 0) - before) < 1, "unassigned, button A does nothing")
    finally:
        if aux:
            aux.stop()
        if imp:
            imp.stop()
        vt.stop()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
