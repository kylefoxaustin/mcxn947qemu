#!/usr/bin/env python3
"""Build a reset-value GOLDEN from a vendor Reference Manual + its CMSIS header.

    ORACLE YOU DID NOT AUTHOR.  That is the entire point.

Every other test in a QEMU device tree is written by the same person who wrote the
model, so a test can agree with the model about something that is not true of the
silicon.  We found the extreme case: an LPI2C sub-block decoded at the wrong offset,
UNREACHABLE FROM THE GUEST, with a green test -- because the test had taken its
addresses FROM THE MODEL.  A test that gets its addresses from the model is not a
test, it is a MIRROR, and mutation testing is blind to it BY CONSTRUCTION (mutate the
model and the mirror moves with it).

This tool builds a golden from two sources the model author did not write:
  * the RM's reset-value column  -> what the register READS at reset
  * the CMSIS header             -> WHERE that register lives
and a companion checker reads every one of them back out of a running machine.

------------------------------------------------------------------------------
USAGE
    pdftotext -f 1 -l 9999 YourChip_RM.pdf rm.txt
    ./extract-rm-golden.py rm.txt YourChip_cm33_core0.h golden.json
------------------------------------------------------------------------------
RM COLUMN FORMAT IT EXPECTS  (NXP MCX / i.MX / RT manuals all print this)

Register-summary tables come out of pdftotext as five consecutive lines:

    200h                                     <- offset
    SIRC Control Status Register (SIRCCSR)   <- description, CMSIS name in parens
    32                                       <- width
    RW                                       <- access
    0100_0020h                               <- RESET VALUE   <-- the golden

Array registers print as a RANGE, on ONE line, and are the thing everyone misses:

    2C0h - 2CCh ADC Trigger Input Connections (ADC1_TRIG0 - ADC1_TRIG3)
    32
    RW
    0000_007Fh

------------------------------------------------------------------------------
⚠ THE GOTCHA FOR THIS TOOL'S CARD -- what a competent session WILL assume and be
  wrong about:

  ⭐ YOU WILL ASSUME IT PROBES EVERY REGISTER.  IT DOES NOT, AND IT WILL NOT TELL
     YOU UNLESS YOU MAKE IT.

  The RM prints ARRAY registers as a range ("2C0h - 2CCh ... (ADC1_TRIG0 -
  ADC1_TRIG3)") and CMSIS stores them as ONE array field (ADC1_TRIG[4] @ 0x2C0).
  A naive parser matches NEITHER -- so EVERY ARRAY REGISTER ON THE CHIP is
  invisible, SILENTLY.  We shipped it that way, ran it, got a confident PASS, and
  the registers we had JUST FIXED were among the ones it could not see.  Expanding
  both sides took our golden 1655 -> 1695 and immediately surfaced 4 more lies.

  So: this tool is itself a candidate for the failure it exists to catch -- a check
  that quietly covers less than it claims is another green light with nothing behind
  it.  Therefore it PRINTS ITS OWN COVERAGE, and you should:

    1. VALIDATE THE PARSER AGAINST KNOWN ANCHORS BEFORE TRUSTING ONE BYTE OF IT.
       Hand-read 2-3 reset values out of the PDF and assert them.  A parser is a
       model of a document, and a model that is its own oracle passes every test.
    2. DROP, DO NOT GUESS.  If (name, offset) maps to more than one peripheral type,
       skip it and COUNT it.  A wrong golden is worse than a missing one: it makes
       the CHECKER lie, and now your oracle is the thing that needs an oracle.
    3. TREAT COVERAGE AS A FLOOR, NOT A CEILING, IN WRITING.

------------------------------------------------------------------------------
AND THE GOTCHA FOR THE CHECKER THAT CONSUMES THIS (learned the expensive way):

  * `-qtest stdio` WITHOUT `-accel qtest` STILL RUNS A TCG vCPU.  It boots from a
    zeroed vector table, faults, and dies of Lockup -- and a free-running CPU is a
    SECOND WRITER to the address space you are reading.  Halt it.
  * THERE IS NO `quit` COMMAND IN THE QTEST PROTOCOL (it answers "FAIL Unknown
    command").  QEMU never exits; THE HARNESS must kill it.  Before we added
    -accel qtest, THE CRASH WAS WHAT ENDED OUR QTESTS -- two defects whose only
    symptom was each other, with a green test on top.
  * ASSERT YOU GOT AS MANY ANSWERS AS YOU ASKED QUESTIONS.  A truncated conversation
    and a correct one differ only in the answers you never notice are missing.
  * THE ALLOWLIST MUST FAIL IN BOTH DIRECTIONS: fail on a new mismatch, AND FAIL WHEN
    AN ALLOWLISTED ENTRY STARTS MATCHING ("this is fixed -- delete the line").  An
    allowlist that never shrinks stops being a to-do list and becomes a CERTIFICATE.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import collections, json, re, sys

SINGLE = re.compile(r'^([0-9A-F]{1,5})h$')
ARRAY  = re.compile(r'^([0-9A-F]{1,5})h\s*-\s*([0-9A-F]{1,5})h\s+.*'
                    r'\(([A-Za-z0-9_]*?)(\d+)\s*-\s*[A-Za-z0-9_]*?(\d+)\)$')
NAME   = re.compile(r'^.*\(([A-Za-z0-9_]+)\)$')
WIDTH  = re.compile(r'^(8|16|32|64)$')
ACCESS = re.compile(r'^(RW|RO|WO|W1C|R|W)$')
RESET  = re.compile(r'^([0-9A-F]{4}_[0-9A-F]{4}|[0-9A-F]{2}_[0-9A-F]{4}|[0-9A-F]{1,16})h$')


def parse_rm(path):
    """(name, offset, width, access, reset) for every register row we can read."""
    lines = [l.strip() for l in open(path, errors="replace")]
    lines = [l for l in lines if l]
    rows, arrays, i = [], 0, 0

    while i < len(lines) - 4:
        a = ARRAY.match(lines[i])
        if a and WIDTH.match(lines[i+1]) and ACCESS.match(lines[i+2]) \
             and RESET.match(lines[i+3]):
            lo, hi = int(a.group(1), 16), int(a.group(2), 16)
            stem, first, last = a.group(3), int(a.group(4)), int(a.group(5))
            n = last - first + 1
            if n > 1 and hi > lo:
                step = (hi - lo) // (n - 1)
                w, acc = int(lines[i+1]), lines[i+2]
                rst = int(lines[i+3].rstrip('h').replace('_', ''), 16)
                for k in range(n):
                    rows.append(("%s%d" % (stem, first + k), lo + k * step, w, acc, rst))
                arrays += 1
                i += 4
                continue
        m = SINGLE.match(lines[i])
        if m:
            nm = NAME.match(lines[i+1])
            if nm and WIDTH.match(lines[i+2]) and ACCESS.match(lines[i+3]) \
                  and RESET.match(lines[i+4]):
                rows.append((nm.group(1), int(m.group(1), 16), int(lines[i+2]),
                             lines[i+3],
                             int(lines[i+4].rstrip('h').replace('_', ''), 16)))
                i += 5
                continue
        i += 1
    return rows, arrays


def parse_cmsis(path):
    """peripheral type -> {register: offset}, plus bases and instance->type.

    ARRAYS ARE EXPANDED.  CMSIS writes `ADC1_TRIG[4]` with one offset and a step;
    the RM names each element.  Miss this and every array register on the chip is
    invisible to the join -- silently.
    """
    src = open(path, errors="replace").read()
    periph = {}
    for m in re.finditer(r'typedef struct \{(.*?)\} (\w+)_Type;', src, re.S):
        regs = {}
        for f in re.finditer(
                r'__[IO]+\s+\w+\s+(\w+)\s*(?:\[(\d+)\])?\s*;\s*/\*\*<[^*]*?'
                r'(?:array )?offset:\s*(0x[0-9A-Fa-f]+)'
                r'(?:[^*]*?array step:\s*(0x[0-9A-Fa-f]+))?', m.group(1)):
            name, n, off = f.group(1), f.group(2), int(f.group(3), 16)
            if n:
                step = int(f.group(4), 16) if f.group(4) else 4
                for k in range(int(n)):
                    regs["%s%d" % (name, k)] = off + k * step
            regs[name] = off
        if regs:
            periph[m.group(2)] = regs

    bases = {m.group(1): int(m.group(2), 16) for m in
             re.finditer(r'#define (\w+)_BASE\s+\(?\(?(0x[0-9A-Fa-f]+)u?\)?', src)}
    inst2type = {}
    for m in re.finditer(r'#define (\w+)_BASE_PTRS\s+\{([^}]*)\}', src):
        for inst in re.findall(r'\b(\w+)\b', m.group(2)):
            if inst in bases:
                inst2type[inst] = m.group(1)
    return periph, bases, inst2type


def main(rm_txt, cmsis_h, out_json):
    rows, arrays = parse_rm(rm_txt)
    periph, bases, inst2type = parse_cmsis(cmsis_h)

    owner = collections.defaultdict(set)
    for t, regs in periph.items():
        for n, o in regs.items():
            owner[(n, o)].add(t)

    golden, unmatched, ambiguous = [], 0, 0
    for name, off, width, acc, reset in rows:
        types = owner.get((name, off))
        if not types:
            unmatched += 1
            continue
        if len(types) > 1:
            ambiguous += 1          # DROP, never guess -- and count what you dropped
            continue
        if width != 32 or acc not in ("RW", "RO", "R"):
            continue
        t = next(iter(types))
        for inst, ity in inst2type.items():
            if ity == t:
                golden.append({"inst": inst, "reg": name,
                               "addr": bases[inst] + off, "reset": reset})

    golden.sort(key=lambda x: (x["inst"], x["addr"]))
    json.dump(golden, open(out_json, "w"), indent=0)

    print("RM rows parsed            : %d  (%d array ranges expanded)" % (len(rows), arrays))
    print("  unmatched in CMSIS      : %d   (RM names a register CMSIS does not, there)" % unmatched)
    print("  ambiguous (>1 periph)   : %d   (DROPPED, not guessed)" % ambiguous)
    print("golden                    : %d registers, %d instances -> %s"
          % (len(golden), len({g['inst'] for g in golden}), out_json))
    print()
    print("⚠ COVERAGE IS A FLOOR, NOT A CEILING.  Rows whose table layout did not")
    print("  parse are INVISIBLE to every check built on this file.  Validate against")
    print("  hand-read anchors before you trust a single byte of it: a parser is a")
    print("  model of a document, and a model that is its own oracle passes every test.")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
