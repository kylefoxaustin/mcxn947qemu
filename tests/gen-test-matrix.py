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

def _reject_unknown_sections(doc):
    """An unknown section key used to be SILENTLY DROPPED — so a typo deleted a
    whole block from the capability table and the anti-drift gate stayed GREEN.
    That is a no-op edit in the tooling itself, which is the one class this
    project keeps getting bitten by.  Refuse it loudly instead."""
    # Non-block schema keys that legitimately live at top level.
    known = set(SECTION_TITLES) | {
        "meta", "absent", "aggregate_harnesses", "class_aliases",
        "readme_absent", "readme_groups",
    }
    unknown = [k for k in doc if k not in known]
    if unknown:
        raise SystemExit(
            "test-matrix.yaml: unknown section key(s) %s.\n"
            "  These would be SILENTLY DROPPED from the capability table and the\n"
            "  drift gate would still pass.  Known sections: %s"
            % (", ".join(sorted(unknown)), ", ".join(sorted(SECTION_TITLES))))


# The only legal values of a row's `result`.  There used to be NO CHECK AT ALL:
# the field took any string I happened to type, and the two one-offs already in the
# file ("operator-run", "partial") had never been agreed anywhere -- they simply
# were not rejected.  A misspelt result silently became "not pass", which quietly
# EXCLUDES THE ROW FROM BEING RUN (see the `result == "pass"` gate below) -- so a
# typo could retire a test from the suite while the drift gate stayed green.
#
#     ⭐ A SCHEMA THAT ACCEPTS ANYTHING VALIDATES NOTHING.  This is the same
#        no-op-edit class as _reject_unknown_sections above, one field over.
RESULTS = {
    "pass",          # the listed suites run and pass in CI
    "partial",       # modelled, but with a NAMED gap in the note -- not run as pass
    "operator-run",  # only verifiable by a human driving the operator interface
    "fail",          # known-broken and admitted
}


def _reject_unknown_result(doc):
    bad = []
    # ONLY the block sections carry a `result`.  readme_groups (and the other
    # schema keys) are lists of something else entirely, and walking them was how
    # the first cut of this guard managed to reject the file it was validating.
    for sect in SECTION_TITLES:
        for r in doc.get(sect) or []:
            if isinstance(r, dict) and r.get("result") not in RESULTS:
                bad.append("%s/%s: result=%r" % (sect, r.get("block"), r.get("result")))
    if bad:
        raise SystemExit(
            "test-matrix.yaml: illegal `result` value(s):\n  %s\n"
            "  Anything that is not 'pass' is silently NOT RUN, so a typo here\n"
            "  retires a test from CI while the drift gate stays green.\n"
            "  Legal: %s" % ("\n  ".join(bad), ", ".join(sorted(RESULTS))))


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
| **honest-fault** | Compute not modelled; the op FAILS to the GUEST through the block's own non-gating error channel, and no result is fabricated. The guest is told, and is never hung. |
| **flag-at-operator** | DEPRECATED — do not use for anything the guest reads. See below. |
| **not-modelled** | Silicon present but the compute register set is not modelled (honest). |

⚠️ **flag-at-operator is a trap, and it used to be defined as a safe endpoint here.**
It read: "op acked + truth exposed via QMP (no silent-wrong)".  That is wrong, and
it licensed real bugs in this tree (the Neutron NPU, the ELS crypto engine, the
SmartDMA, PowerQuad's unmodelled opcodes).  QMP and the host log are exposure to
the OPERATOR.  The firmware under test cannot see either.  If the guest CONSUMES
the result — and it always does when the result travels by pointer into a buffer
the guest supplied — then acking the operation is a SILENT WRONG ANSWER to the
guest, no matter how loudly the host is told.

**Being honest to the host while lying to the guest is not being honest.**

Anything the guest reads must be **honest-fault**: fail through the block's own
documented error channel (never the completion gate, which hangs the driver
instead of informing it).  flag-at-operator is acceptable only for pure telemetry
that no guest ever reads.  (Tightened in step with 95emulator, who found the same
defect in their class enum: the rule is where it hides.)

Fleet class aliases (for cross-repo diff): computes≈COMPUTES,
operator-driven≈HONEST-PARAMETERIZABLE, honest-fault≈HONEST-FAULT,
register-only≈registration/Tier-C.
"""

# yaml class -> condensed README tier.  A = real data/math verified (data-path);
# B = register-accurate bring-up (operator-driven analog, honest-flagged accels,
# config/security/clock registers).  The README table renders these; the detailed
# matrix renders the full class.  One source of truth, two renderings.
CLASS_TIER = {
    "computes": "A", "functional": "A",
    "operator-driven": "B", "register-only": "B",
    "honest-fault": "B", "flag-at-operator": "B", "not-modelled": "B",
}


def _reject_flag_at_operator(doc):
    """⛔ flag-at-operator is DEPRECATED AND WAS ITSELF THE BUG.

    It was defined as a safe endpoint -- "op acked + truth exposed via QMP (no
    silent-wrong)" -- but QMP and the host log reach the OPERATOR.  THE FIRMWARE
    UNDER TEST CANNOT SEE THEM.  From inside the guest, an acked op with an
    untouched result buffer IS a silent wrong answer.  Being honest to the host
    while lying to the guest is not being honest.

    That sentence, written down as policy, authorised FOUR real bugs in this tree
    (Neutron, ELS, SmartDMA, PowerQuad).  Deprecating it in prose is not enough --
    a rule you merely DOCUMENT as retired is still loaded.  REFUSE IT.

    Anything a guest reads must be `honest-fault`: fail through the block's own
    documented, NON-GATING error channel (never the completion gate, which hangs
    the driver instead of informing it).
    """
    bad = [b["block"]
           for k, v in doc.items() if isinstance(v, list)
           for b in v
           if isinstance(b, dict) and b.get("class") == "flag-at-operator"]
    if bad:
        raise SystemExit(
            "test-matrix.yaml: class 'flag-at-operator' is DEPRECATED and REFUSED "
            "-- used by: %s.\n"
            "  It means 'the op is acked and the truth goes to QMP'.  QMP reaches "
            "the OPERATOR;\n"
            "  the FIRMWARE cannot see it.  From inside the guest that IS a silent "
            "wrong answer,\n"
            "  and this rule already authorised four real bugs here.\n"
            "  Use 'honest-fault': fail to the GUEST through the block's own "
            "non-gating error channel."
            % ", ".join(sorted(bad)))

README = os.path.join(ROOT, "README.md")
README_BEGIN = "<!-- BEGIN capability-table (generated from test-matrix.yaml) -->"
README_END = "<!-- END capability-table (generated from test-matrix.yaml) -->"


def present_tiers(doc):
    """Flatten every present block across the block sections -> {block: tier}."""
    out = {}
    for key in SECTION_TITLES:
        for b in doc.get(key) or []:
            if b.get("present", True):
                cls = b.get("class")
                if cls not in CLASS_TIER:
                    raise SystemExit(f"block '{b['block']}': unknown class '{cls}'")
                out[b["block"]] = CLASS_TIER[cls]
    return out


def verify_groups(doc):
    """Anti-drift guard: every present block is grouped into exactly one
    single-tier readme_group whose tier matches the block's class-derived tier.
    So the condensed README table and the detailed matrix physically cannot
    diverge — a block added / removed / re-classed in the YAML fails generation
    until its readme_group is updated too."""
    present = present_tiers(doc)
    seen, errs = [], []
    for g in doc.get("readme_groups", []):
        for b in g["blocks"]:
            seen.append(b)
            if b not in present:
                errs.append(f"readme_group '{g['label']}' lists unknown/absent block {b}")
            elif present[b] != g["tier"]:
                errs.append(f"'{b}' is tier {present[b]} but group "
                            f"'{g['label']}' is tier {g['tier']}")
    for b in present:
        n = seen.count(b)
        if n != 1:
            errs.append(f"present block '{b}' grouped {n}x (want exactly 1)")
    if errs:
        raise SystemExit("readme_group drift:\n  " + "\n  ".join(errs))


def render_readme_capability(doc):
    out = ["| Subsystem | Tier | Evidence |", "|---|:--:|---|"]
    for g in doc.get("readme_groups", []):
        out.append(f"| {g['label']} | {g['tier']} | {g.get('evidence', '')} |")
    out += ["", "**Absent on MCXN947 silicon — N/A (never a failure):**", "",
            "| Block | Why absent |", "|---|---|"]
    for g in doc.get("readme_absent", []):
        out.append(f"| {g['label']} | {g['reason']} |")
    return "\n".join(out)


def inject_readme(doc, path):
    verify_groups(doc)
    text = open(path).read()
    if README_BEGIN not in text or README_END not in text:
        raise SystemExit(f"{path}: missing capability-table markers")
    head, rest = text.split(README_BEGIN, 1)
    _, tail = rest.split(README_END, 1)
    block = f"{README_BEGIN}\n{render_readme_capability(doc)}\n{README_END}"
    new = head + block + tail
    if new != text:
        open(path, "w").write(new)
        sys.stderr.write(f"updated {path} capability table from test-matrix.yaml\n")
        return 1
    sys.stderr.write(f"{path} capability table already in sync\n")
    return 0


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
    ap.add_argument("--inject-readme", nargs="?", const=README,
                    help="regenerate the README capability table from the YAML "
                         "(between the capability-table markers) and exit")
    a = ap.parse_args()

    doc = yaml.safe_load(open(YAML))
    _reject_unknown_sections(doc)
    _reject_flag_at_operator(doc)
    _reject_unknown_result(doc)
    verify_groups(doc)   # structural anti-drift: always, cheap
    if a.inject_readme is not None:
        sys.exit(inject_readme(doc, a.inject_readme))
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
