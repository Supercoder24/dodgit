"""
Runs the scenarios of tests/sim/scenarios/*.py (called by tests/sim/run.sh, which builds the simulator and sets
SIM_BUILD). Every scenario runs in its own Python process (a crash or a hang of one cannot hide the others),
several at a time.

    run_scenarios.py                     run the CORE scenarios (a small, fast, representative set)
    run_scenarios.py --all               run every scenario (the soak test takes the longest)
    run_scenarios.py [substring ...]     run the scenarios (of all) whose name contains one of the substrings
    run_scenarios.py --list              list them; core ones are marked with *
"""
import concurrent.futures
import glob
import importlib.util
import os
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import simlib  # noqa: E402


# The default set: one or two scenarios per behaviour of the plan, chosen to stay quick. Everything else is
# run with --all or by name.
CORE = [
    "start_ball_first_press",         # Roll Call, slots, REMEMBER, ranging distances
    "start_pouches_first_press",      # power-on order does not matter
    "pouch_cycle_lives_2",            # a rebooted pouch gets its slot AND lives back
    "pouch_cycle_single_pouch_game",  # ... also when it is the only pouch (nobody answers while it is away)
    "ball_reboot_pouches_running",    # ball reboot: pouches fall back to Joining and re-register
    "lost_hit_all_remember_fixes",    # the ball's table is authoritative: REMEMBER repairs a lost HIT
    "late_pouch_joins_at_reset",      # hold the button: the late pouch joins, lives reset
    "late_pouch_first_reset_lost",    # ... even if the first RESET broadcast is lost
    "late_pouch_joins_in_last_200ms", # ... and a pouch that joins at the very end is not deleted
    "reset_deletes_silent_pouch",     # silent pouches are deleted at the end of the Reset
    "flow_pickup",                    # Held + near -> Ready
    "flow_hit_other_pouch_loses_one_life",
    "flow_hit_out_of_lives_is_miss",
    "flow_short_throw_ignored",       # no point-blank throws
    "flow_hit_bounce_one_life",       # hit then bounce = one life
    "classify_pouch_silent_in_one_round",
    "flow_launch_spike_is_not_a_hit", # the launch spike is drained in Ready, not classified
    "flow_close_players_do_not_swap_thrower",
    "reset_from_ready",               # a Reset in the middle of the game: lives restored, nobody deleted
    "reset_from_thrown",
    "reset_from_hit",
]


def load_scenarios():
    for path in sorted(glob.glob(os.path.join(HERE, "scenarios", "*.py"))):
        spec = importlib.util.spec_from_file_location("scn_" + os.path.basename(path)[:-3], path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    unknown = [n for n in CORE if n not in simlib.SCENARIOS]
    if unknown:
        raise SystemExit("CORE lists scenarios that do not exist: %s" % unknown)
    return simlib.SCENARIOS


def run_one(name):
    """Child process: run one scenario, print its notes and (on failure) the explanation."""
    sc = load_scenarios()[name]
    w = simlib.World(seed=sc.get("seed", 1), name=name)
    t0 = time.time()
    try:
        sc["fn"](w)
    except simlib.Fail as e:
        print("FAILED after %.1f s wall, virtual time %.2f s" % (time.time() - t0, w.now))
        print(e)
        return 1
    except Exception:
        print("ERROR (the scenario itself is broken, or the simulator is): virtual time %.2f s" % w.now)
        traceback.print_exc()
        return 2
    print("ok: %.1f s wall, %.1f s virtual, %d frames" % (time.time() - t0, w.now, w.stat("frames")))
    return 0


def indent(text, pad="      "):
    return "\n".join(pad + ln for ln in text.splitlines())


def main(argv):
    if len(argv) >= 3 and argv[1] == "--one":
        return run_one(argv[2])
    scenarios = load_scenarios()
    if argv[1:2] == ["--list"]:
        for n, sc in scenarios.items():
            print(("* " if n in CORE else "  ") + n + ("   [KNOWN BUG #%s]" % sc["known_bug"] if sc["known_bug"] else ""))
        return 0
    run_all = "--all" in argv[1:]
    filt = [a for a in argv[1:] if a != "--all"]
    if filt:
        names = [n for n in scenarios if any(f in n for f in filt)]
    else:
        names = list(scenarios) if run_all else [n for n in scenarios if n in CORE]
    if not names:
        print("no scenario matches %s" % filt)
        return 2
    jobs = int(os.environ.get("SIM_JOBS", "0")) or os.cpu_count() or 2
    verbose = os.environ.get("SIM_VERBOSE") == "1"
    env = dict(os.environ)

    def job(name):
        t0 = time.time()
        try:
            p = subprocess.run([sys.executable, "-I", os.path.join(HERE, "run_scenarios.py"), "--one", name],
                               capture_output=True, text=True, env=env, timeout=scenarios[name]["timeout"])
            out, rc = p.stdout + p.stderr, p.returncode
        except subprocess.TimeoutExpired as e:
            out, rc = "TIMEOUT after %d s\n%s" % (scenarios[name]["timeout"], e.stdout or ""), 3
        return name, rc, out, time.time() - t0

    results = []
    t_all = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for name, rc, out, dt in ex.map(job, names):
            bug = scenarios[name]["known_bug"]
            if rc == 0:
                tag = "PASS"
                extra = "   (known bug #%s is fixed: remove the mark)" % bug if bug else ""
            else:
                tag = "FAIL"
                extra = "   KNOWN BUG #%s" % bug if bug else ""
                if rc < 0:
                    extra += "   (crashed: signal %d)" % -rc
            print("%s %-44s %5.1f s%s" % (tag, name, dt, extra))
            if rc != 0 or verbose:
                print(indent(out.rstrip()))
            results.append((name, rc, bug))
    failed = [r for r in results if r[1] != 0]
    known = [r for r in failed if r[2]]
    unknown = [r for r in failed if not r[2]]
    print("\n%d scenarios, %d passed, %d failed (%d known bugs), %.0f s" %
          (len(results), len(results) - len(failed), len(failed), len(known), time.time() - t_all))
    if unknown:
        return 1
    if known:
        return 0 if os.environ.get("SIM_ALLOW_KNOWN") == "1" else 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
