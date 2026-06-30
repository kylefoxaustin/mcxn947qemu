#!/usr/bin/env python3
"""
Generate docs/validation/test-result-matrix.md from test-matrix.yaml + a real
test run, and gate on regressions.

Design (matches the fleet CI guardrail, 95/91/93):
  - test-matrix.yaml is the SOURCE OF TRUTH for the human-judgment columns
    (present, driver-binds, class, tested-by).  They are copied VERBATIM —
    the generator never infers a class or tier from a green test.
  - The `result` column is filled from an ACTUAL run of the mcxn-* suites.
  - The human matrix (.md) is regenerated from the yaml sections.
  - Exit non-zero on a regression: any block/harness the yaml marks `pass`
    whose tests actually FAIL (CI gate).

Usage:
  tests/gen-test-matrix.py            # run the suite, regenerate the md, check
  tests/gen-test-matrix.py --no-run   # regenerate from cached results only
  tests/gen-test-matrix.py --check    # also exit non-zero on regression
"""
import os, sys, subprocess, time, argparse

try:
    import yaml
except ImportError:
    print("PyYAML required (pip install pyyaml)"); sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
YAML = os.path.join(ROOT, "docs/validation/test-matrix.yaml")
MD = os.path.join(ROOT, "docs/validation/test-result-matrix.md")

# yaml section key -> human heading (block sections, in display order).
SECTION_TITLES = {
    "cores": "Cores",
    "connectivity": "Connectivity & data-path peripherals",
    "timers": "Timers",
    "compute": "Compute / accelerators",
    "analog": "Analog (operator-driven)",
    "gpio_dma_irq": "GPIO / pin / DMA / interrupt",
    "memory_flash_cache": "Memory / flash / cache",
    "clock_power_system": "Clock / power / system",
    "security_crypto_tamper": "Security / crypto / tamper",
    "usb_support": "USB support blocks",
}
# Pseudo tested-by tokens that map to a covering boot/corpus suite.
PSEUDO = {"boot": "mcxn-zephyr", "corpus": "mcxn-mcuxpresso"}

# Per-suite wall-clock budget.
DEFAULT_TIMEOUT = 120
SUITE_TIMEOUT = {"mcxn-mcuxpresso": 300}

# Heavy aggregate meta-suites that boot many ELFs back-to-back (minutes, and
# load-sensitive): not re-run on every generator invocation — they get their
# own CI job.  Recorded as "(separate)"; never gate the fast per-device run.
# Pass --with-slow to include them.
SLOW_SEPARATE = {"mcxn-ztest"}

LEGEND = """## Class legend

| Class | Meaning |
|-------|---------|
| **computes** | Does the real work; results match silicon (within emulation). |
| **functional** | Moves real data / generates real events + IRQs on a verified data path. |
| **operator-driven** | No physical stimulus in QEMU; the input is a runtime QOM property (analog). |
| **register-only** | Register-accurate; no compute expected (config / cache / ID / security-trim). |
| **flag-at-operator** | Proprietary compute not modelled; op acked + truth exposed via QMP (no silent-wrong). |
| **not-modelled** | Silicon present but the compute register set is not modelled (honest). |

Fleet class aliases (for cross-repo diff): computes≈COMPUTES,
operator-driven≈HONEST-PARAMETERIZABLE, flag-at-operator≈FAULTS-ABSENT,
register-only≈registration/Tier-C.
"""


def real_tests(doc):
    """Unique mcxn-* suites referenced by any tested_by (pseudo mapped)."""
    tests = set()
    for sec in doc.values():
        if not isinstance(sec, list):
            continue
        for row in sec:
            for t in row.get("tested_by", []):
                t = PSEUDO.get(t, t)
                if t.startswith("mcxn-"):
                    tests.add(t)
    return sorted(tests)


def run_suite(tests, with_slow):
    """Run each suite once; return {test: PASS|SKIP|FAIL|SEPARATE}."""
    results = {}
    for i, t in enumerate(tests):
        if t in SLOW_SEPARATE and not with_slow:
            results[t] = "SEPARATE"
            print(f"  {t}: SEPARATE (heavy meta-suite, own CI job)")
            continue
        rs = os.path.join(ROOT, "tests", t, "run.sh")
        if not os.path.exists(rs):
            results[t] = "SKIP"
            continue
        env = dict(os.environ, USB_PORT=str(17000 + i))
        try:
            out = subprocess.run(["bash", rs], cwd=ROOT, env=env,
                                  capture_output=True, text=True,
                                  timeout=SUITE_TIMEOUT.get(t, DEFAULT_TIMEOUT),
                                  stdin=subprocess.DEVNULL).stdout
        except subprocess.TimeoutExpired:
            results[t] = "FAIL"; print(f"  {t}: TIMEOUT"); continue
        last = "FAIL"
        for line in out.splitlines():
            s = line.strip()
            if s in ("PASS", "SKIP", "FAIL") or s.startswith(("PASS", "FAIL", "SKIP")):
                last = s.split()[0]
        results[t] = last
        print(f"  {t}: {last}")
    return results


def row_result(row, run):
    tb = [PSEUDO.get(t, t) for t in row.get("tested_by", [])]
    real = [run.get(t) for t in tb if t.startswith("mcxn-")]
    if not real:
        return "—"
    if "FAIL" in real:
        return "FAIL"
    if any(r == "PASS" for r in real):
        return "PASS"               # a live pass covers it (SEPARATE/SKIP ignored)
    if "SEPARATE" in real:
        return "(separate)"         # only covered by a heavy meta-suite
    return "SKIP"


def render(doc, run):
    L = ["# MCXN947 — test / result matrix", "",
         "_Generated by tests/gen-test-matrix.py from test-matrix.yaml + a real "
         "run. Edit the yaml (human-judgment columns), not this file._", "",
         LEGEND]
    for key, title in SECTION_TITLES.items():
        rows = doc.get(key)
        if not rows:
            continue
        L += [f"## {title}", "",
              "| Block | Present | Driver-binds | Class | Tested-by | Result |",
              "|-------|:---:|:---:|-------|-----------|:---:|"]
        for r in rows:
            present = "✓" if r.get("present") else "✗"
            db = "✓" if r.get("driver_binds") else "◐"
            tb = ", ".join(r.get("tested_by", [])) or "—"
            L.append(f"| {r['block']} | {present} | {db} | {r['class']} | "
                     f"{tb} | {row_result(r, run)} |")
        L.append("")
    # Absent silicon — present-but-empty for MCX, kept for fleet diff-ability.
    absent = doc.get("absent")
    L += ["## Absent (not present on MCXN947)", ""]
    if not absent:
        L += ["_None — MCXN947 is a superset MCU; every block above is present "
              "silicon (so nothing is ever a false 'negative' here)._", ""]
    else:
        L += ["| Block | Reason |", "|-------|--------|"]
        L += [f"| {r['block']} | {r.get('reason', '')} |" for r in absent]
        L.append("")

    # aggregate harnesses
    harn = doc.get("aggregate_harnesses")
    if harn:
        L += ["## Aggregate harnesses (cross-block)", "",
              "| Harness | Covers | Tested-by | Result |",
              "|---------|--------|-----------|:---:|"]
        for h in harn:
            tb = ", ".join(h.get("tested_by", [])) or "—"
            res = row_result(h, run) if h.get("result") != "operator-run" \
                else "(operator-run)"
            L.append(f"| {h['harness']} | {h['covers']} | {tb} | {res} |")
        L.append("")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-run", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--with-slow", action="store_true",
                    help="also run heavy meta-suites (ztest) inline")
    a = ap.parse_args()

    doc = yaml.safe_load(open(YAML))
    tests = real_tests(doc)
    run = {}
    if not a.no_run:
        print(f"Running {len(tests)} suites...")
        run = run_suite(tests, a.with_slow)

    open(MD, "w").write(render(doc, run))
    print(f"Wrote {MD}")

    # Regression gate: any row the yaml marks pass whose tests actually FAIL.
    regressions = []
    for sec in doc.values():
        if not isinstance(sec, list):
            continue
        for r in sec:
            name = r.get("block") or r.get("harness")
            if r.get("result") == "pass" and not a.no_run \
               and row_result(r, run) == "FAIL":
                regressions.append(name)
    fails = [t for t, v in run.items() if v == "FAIL"]
    if fails:
        print("FAILED suites:", ", ".join(fails))
    if regressions:
        print("REGRESSIONS:", ", ".join(regressions))
    if a.check and (fails or regressions):
        sys.exit(1)
    if not a.no_run:
        npass = sum(1 for v in run.values() if v == "PASS")
        print(f"Summary: {npass}/{len(run)} suites PASS")


if __name__ == "__main__":
    main()
