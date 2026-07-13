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
RANGE  = re.compile(r'^([0-9A-F]{1,5})h\s*-\s*([0-9A-F]{1,5})h$')

# "(ADC1_TRIG0 - ADC1_TRIG3)"  and also INFIX indices: "(P0DR - P31DR)".
#
# ⚠ THE SUFFIX MUST CONTAIN NO DIGITS.  That single constraint is what forces (\d+)
# to be the LAST digit run, and it is the difference between a working regex and a
# silently broken one:
#
#   A non-greedy prefix grabs the FIRST digit run, so "ADC1_TRIG0" splits as
#   ADC / 1 / _TRIG0 -- and the two ends' suffixes then disagree (_TRIG0 vs _TRIG3),
#   so the row REFUSES TO EXPAND and every ADCn_TRIG register vanishes from the
#   golden.  I wrote exactly that regex while adding rt1180emulator's infix support,
#   AND MY OWN ANCHOR GATE CAUGHT IT ON THE NEXT RUN -- a register that used to be in
#   the golden was not.  THE GATE CAUGHT A BUG IN THE GATE.  That is the whole reason
#   the anchors exist, and it is why they must be HAND-READ and never taken from the
#   tool's own output.
#
# prefix AND suffix must agree at both ends, or you are expanding a coincidence.
ELEMS = re.compile(r'\(([A-Za-z0-9_]+)\s*-\s*([A-Za-z0-9_]+)\)\s*$')

# An array row where the RANGE and the NAME share a line.
ARRAY  = re.compile(r'^([0-9A-F]{1,5})h\s*-\s*([0-9A-F]{1,5})h\s+(.*)$')
NAME   = re.compile(r'^.*\(([A-Za-z0-9_]+)\)$')
WIDTH  = re.compile(r'^(8|16|32|64)$')
ACCESS = re.compile(r'^(RW|RO|WO|W1C|R|W)$')
RESET  = re.compile(r'^([0-9A-F]{4}_[0-9A-F]{4}|[0-9A-F]{2}_[0-9A-F]{4}|[0-9A-F]{1,16})h$')


_RUNS = re.compile(r'\d+|\D+')


def _index_of(name_a, name_b):
    """Which run of digits is the ARRAY INDEX?  Answer: THE ONE THAT VARIES.

    ⭐ DO NOT ASK WHERE THE INDEX IS.  ASK WHAT ACTUALLY CHANGES.

    rt1180emulator and I each guessed, from our own manual's habits, and each was
    SILENTLY WRONG ON THE OTHER'S SILICON:

        "index = the FIRST digit run"  -> breaks  ADC1_TRIG0 - ADC1_TRIG3
                                          (splits as ADC / 1 / _TRIG0; drops all of them)
        "index = the LAST digit run"   -> breaks  CLOCK_ROOT0_STATUS0
                                                  - CLOCK_ROOT73_STATUS0
                                          (the varying run is in the MIDDLE, and the name
                                           ALSO ENDS IN A DIGIT; drops all 74)

    HIS FIX FOR MY BUG HAD MY BUG, POINTING THE OTHER WAY.  We each hard-coded our own
    document's habits and called it a parser.

        ⭐ THE ANSWER IS NOT IN THE NAME.  IT IS IN THE PAIR.

    So: split BOTH endpoints into runs of digits/non-digits and compare them.  EXACTLY
    ONE digit run may differ -- that is the index, wherever it happens to sit.  Anything
    else is REFUSED:

        CTX0_CTR0 - CTX3_CTR1   -> TWO runs vary.  It is a 2-D array, NO SINGLE STRIDE
                                   DESCRIBES IT, and any regex that "finds the index"
                                   would confidently emit a wrong one.  DROP, AND COUNT.

    A WRONG GOLDEN MAKES THE CHECKER LIE, AND THEN YOUR ORACLE IS THE THING THAT NEEDS
    AN ORACLE.  This cannot be wrong about where the index is, BECAUSE IT NEVER HAS TO
    DECIDE.  (rt1180emulator, b4f99a0917.)

    Returns (runs, position, first, last) or None.
    """
    ra = _RUNS.findall(name_a)
    rb = _RUNS.findall(name_b)
    if len(ra) != len(rb):
        return None

    diff = [i for i, (x, y) in enumerate(zip(ra, rb)) if x != y]
    if len(diff) != 1:
        return None                  # 0 = not a range at all; >1 = 2-D array, REFUSE
    i = diff[0]
    if not (ra[i].isdigit() and rb[i].isdigit()):
        return None                  # the thing that varies is not an index
    return ra, i, int(ra[i]), int(rb[i])


def _expand(lo, hi, desc, width, acc, reset):
    """Expand one array row into its elements, or return [] if it isn't one."""
    m = ELEMS.search(desc)
    if not m:
        return []
    got = _index_of(m.group(1), m.group(2))
    if not got:
        return []
    runs, pos, first, last = got
    n = last - first + 1
    if n < 2 or hi <= lo:
        return []
    step = (hi - lo) // (n - 1)

    out = []
    for k in range(n):
        parts = list(runs)
        parts[pos] = str(first + k)
        out.append(("".join(parts), lo + k * step, width, acc, reset))
    return out


def parse_rm(path):
    """(name, offset, width, access, reset) for every register row we can read.

    Also returns the DEFERRED list: rows that parse in EVERY column EXCEPT the reset
    value, because the manual writes "See section" there instead of a number.

    ⭐ A REFUSAL IS NOT A CHECK -- AND AN UNCOUNTED REFUSAL IS NOT EVEN A REFUSAL.
                                                                (91emulator)

    Those rows never matched the row pattern at all.  They were not dropped, they were
    INVISIBLE: not in the golden, not in the refusal list, not in ANY number this tool
    printed.  And the RM writes "See section" for a reason -- usually because THE RESET
    VALUE DEPENDS ON THE INSTANCE, which is precisely the shape that was hiding the PORT
    PCR pads (PORT0 = 0x1143 = the SWD debug pins, PORT1-5 = 0).  I found those BY
    ACCIDENT, chasing a different lead, because my own tool could not tell me they
    existed.

    They are now COUNTED and PRINTED, so the next one is found on purpose.
    """
    lines = [l.strip() for l in open(path, errors="replace")]
    lines = [l for l in lines if l]
    rows, arrays, i = [], 0, 0
    deferred = []

    while i < len(lines) - 5:
        # (a) ARRAY, range and name on ONE line:
        #        "2C0h - 2CCh ADC Trigger Input Connections (ADC1_TRIG0 - ADC1_TRIG3)"
        a = ARRAY.match(lines[i])
        if a and WIDTH.match(lines[i+1]) and ACCESS.match(lines[i+2]) \
             and RESET.match(lines[i+3]):
            got = _expand(int(a.group(1), 16), int(a.group(2), 16), a.group(3),
                          int(lines[i+1]), lines[i+2],
                          int(lines[i+3].rstrip('h').replace('_', ''), 16))
            if got:
                rows.extend(got)
                arrays += 1
                i += 4
                continue

        # (b) ARRAY, range and name on SEPARATE lines:
        #        "80h - FCh"
        #        "Interrupt Control Register a (ICR0 - ICR31)"
        #     ⚠ rt1180emulator hit this porting the tool: the one-line regex matched
        #     NOTHING in his manual, the tool printed "0 array ranges expanded" and a
        #     CONFIDENT GOLDEN with every array register on the chip missing.  That is
        #     THIS FILE'S OWN GOTCHA -- "you will assume it probes every register" --
        #     landing inside the port of the fix for it.  A parser is a model of a
        #     document, AND IT IS A DIFFERENT DOCUMENT.
        r = RANGE.match(lines[i])
        if r and WIDTH.match(lines[i+2]) and ACCESS.match(lines[i+3]) \
             and RESET.match(lines[i+4]):
            got = _expand(int(r.group(1), 16), int(r.group(2), 16), lines[i+1],
                          int(lines[i+2]), lines[i+3],
                          int(lines[i+4].rstrip('h').replace('_', ''), 16))
            if got:
                rows.extend(got)
                arrays += 1
                i += 5
                continue

        # (c) a plain single register.
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

        # (d) THE DEFERRED ROWS.  Everything parses except the reset cell, because the
        #     manual says "See section" -- typically because the value is PER-INSTANCE.
        #     Count them.  An uncounted refusal is not even a refusal.
        m2 = SINGLE.match(lines[i]) or RANGE.match(lines[i])
        if m2:
            nm = NAME.match(lines[i+1])
            if nm and WIDTH.match(lines[i+2]) and ACCESS.match(lines[i+3]) \
                  and not RESET.match(lines[i+4]):
                deferred.append((nm.group(1), lines[i+4]))
        i += 1
    return rows, arrays, deferred


# A struct array inside a peripheral typedef:   "  } CH[16];"
STRUCT_ARRAY_END = re.compile(r'^\s*\}\s*(\w+)\[(\w+)\]\s*;', re.M)


def _member_names(field, struct_name, idx):
    """Candidate RM names for element `idx` of a STRUCT ARRAY member.

    ⚠ THERE IS NO SINGLE CONVENTION, SO DO NOT GUESS ONE.

    CMSIS declares the eDMA channel block as `struct { ... } CH[16]`, and the RM
    names its members TWO different ways in the SAME struct:

        CMSIS  CH_CSR   (in CH[16])  ->  RM  CH0_CSR  ... CH15_CSR
        CMSIS  TCD_CSR  (in CH[16])  ->  RM  TCD0_CSR ... TCD15_CSR

    -- the index goes after the FIRST token, and the token is NOT the struct's name.
    Other peripherals append it instead (ADC's CMD[15] -> CMDL1, CMDH1, 1-BASED).

    So EMIT EVERY PLAUSIBLE CANDIDATE AND LET THE (name, offset) JOIN DECIDE.  A wrong
    candidate name matches NO RM row and is harmless -- the offset has to agree too, so
    this can only ADD CORRECT JOINS, NEVER INVENT ONE.  Guessing a single convention is
    what would produce a wrong golden, and A WRONG GOLDEN MAKES THE CHECKER LIE.
    """
    out = set()
    head, sep, tail = field.partition("_")
    if sep:
        out.add("%s%d_%s" % (head, idx, tail))       # CH_CSR -> CH0_CSR; TCD_CSR -> TCD0_CSR
    out.add("%s%d" % (field, idx))                   # CMDL   -> CMDL0
    out.add("%s%d" % (field, idx + 1))               # CMDL   -> CMDL1  (1-based)
    out.add("%s%d_%s" % (struct_name, idx, field)) if False else None
    return out


def _count(tok, src):
    """Element count: a literal, or a #define'd COUNT macro."""
    if tok.isdigit():
        return int(tok)
    m = re.search(r'#define\s+%s\s+\(?(\d+)' % re.escape(tok), src)
    return int(m.group(1)) if m else 0


def parse_cmsis(path):
    """peripheral type -> {register: offset}, plus bases and instance->type.

    ARRAYS ARE EXPANDED -- BOTH KINDS, and missing either one silently blinds the gate
    to an entire register class:

      * FIELD arrays   `__IO uint32_t ADC1_TRIG[4];`
      * STRUCT arrays  `struct { __IO uint32_t CH_CSR; ... } CH[16];`

    ⭐ THE SECOND ONE HID THE ENTIRE eDMA CHANNEL BLOCK FROM THIS GATE -- the very
    block in which this project found more silent-wrong-answer bugs than any other
    (a dead ERQ, a START that ran the whole major loop, a dropped NBYTES remainder, an
    unmodelled ATTR modulo).  THE GATE THAT EXISTS TO CATCH THAT CLASS COULD NOT LOOK
    AT IT.  rt1180emulator hit the identical blindness on his CCM clock roots and named
    it; both of us had it, in the same week, in the same tool.
    """
    src = open(path, errors="replace").read()
    periph = {}
    for m in re.finditer(r'typedef struct \{(.*?)\} (\w+)_Type;', src, re.S):
        body = m.group(1)
        regs = {}
        collide = set()

        def _put(name, off):
            # regs maps name -> SET of offsets.  Nothing is overwritten, nothing is
            # dropped; the (name, offset) join picks the right one.
            regs.setdefault(name, set()).add(off)

        # (a) plain fields and FIELD arrays.
        for f in re.finditer(
                r'__[IO]+\s+\w+\s+(\w+)\s*(?:\[(\d+)\])?\s*;\s*/\*\*<[^*]*?'
                r'(?:array )?offset:\s*(0x[0-9A-Fa-f]+)'
                r'(?:[^*]*?array step:\s*(0x[0-9A-Fa-f]+))?', body):
            name, n, off = f.group(1), f.group(2), int(f.group(3), 16)
            if n:
                step = int(f.group(4), 16) if f.group(4) else 4
                for k in range(int(n)):
                    _put("%s%d" % (name, k), off + k * step)
            _put(name, off)

        # (b) STRUCT arrays.  Each inner field already carries its own
        #     "array offset" and "array step" (the step being the STRUCT's stride),
        #     so we only need the element COUNT and the member names.
        for sa in STRUCT_ARRAY_END.finditer(body):
            sname, count = sa.group(1), sa.group(2)
            n = _count(count, src)
            if not n:
                continue
            # fields declared before this closing brace, with an array step
            for f in re.finditer(
                    r'__[IO]+\s+\w+\s+(\w+)\s*;\s*/\*\*<[^*]*?'
                    r'array offset:\s*(0x[0-9A-Fa-f]+)'
                    r'[^*]*?array step:\s*(0x[0-9A-Fa-f]+)',
                    body[:sa.start()]):
                fname = f.group(1)
                base, step = int(f.group(2), 16), int(f.group(3), 16)
                for k in range(n):
                    for cand in _member_names(fname, sname, k):
                        if cand not in regs:
                            _put(cand, base + k * step)

        #
        # ⚠ A NAME MAY COLLIDE WITH ITSELF INSIDE CMSIS, AND THE LOSER IS SILENT.
        #
        # ⭐ CORRECTION FROM rt1180emulator, AND HE IS RIGHT: DO NOT DROP THE PAIR.
        #    "The join is on (name, OFFSET), and THE RM ROW CARRIES ITS OWN OFFSET AND
        #     PICKS ONE.  Keep every pair."
        #    My first fix here REFUSED the colliding name outright, which cured the false
        #    witness by throwing away a register that was perfectly checkable.  The cure
        #    is to STOP OVERWRITING, not to start REFUSING: keep BOTH (name, offset)
        #    entries and let the RM's own offset disambiguate.  A DROP IS A GAP; ONLY AN
        #    OVERWRITE IS A LIE.
        #
        # ChipIdea's USBHS declares a SCALAR `ENDPTCTRL0` at 0x1C0 AND an array
        # `ENDPTCTRL[7]` starting at 0x1C4.  Expanding the array emits "ENDPTCTRL0"
        # at 0x1C4 -- the SAME NAME as the scalar, at a DIFFERENT ADDRESS -- and a
        # plain dict keeps whichever was written last.  That produced a golden entry
        # for ENDPTCTRL0 at the WRONG ADDRESS, and the gate then reported a "lie" in
        # a register that was perfectly correct.
        #
        #     ⭐ THAT IS THE ONE THING THIS TOOL MUST NEVER DO.  A WRONG GOLDEN MAKES
        #        THE CHECKER LIE, AND THEN YOUR ORACLE IS THE THING THAT NEEDS AN
        #        ORACLE.  A missing register is a gap; a wrong one is a false witness.
        #
        # So: build with collision detection, and DROP any name CMSIS gives two
        # different offsets.  Drop, don't guess -- the same rule already applied to
        # RM-side contradictions and to >1-type ambiguity.
        #
        if regs:
            periph[m.group(2)] = regs

    #
    # ⚠ TRUSTZONE DEFINES EVERY BASE TWICE -- secure (0x5xxx_xxxx) and non-secure
    # (0x4xxx_xxxx) -- and a dict comprehension keeps WHICHEVER THE FILE DEFINES LAST.
    # Ours landed on the non-secure alias BY ACCIDENT OF FILE ORDERING.  rt1180emulator
    # caught this porting the tool ("derived, not lucky") and he is right: take the
    # LOWER of any pair that differs by exactly the secure-address offset.  A result
    # that is correct by luck is a result you have not checked.
    #
    bases = {}
    for m in re.finditer(r'#define (\w+)_BASE\s+\(?\(?(0x[0-9A-Fa-f]+)u?\)?', src):
        name, val = m.group(1), int(m.group(2), 16)
        if name in bases:
            bases[name] = min(bases[name], val)   # non-secure alias
        else:
            bases[name] = val
    inst2type = {}
    for m in re.finditer(r'#define (\w+)_BASE_PTRS\s+\{([^}]*)\}', src):
        for inst in re.findall(r'\b(\w+)\b', m.group(2)):
            if inst in bases:
                inst2type[inst] = m.group(1)
    return periph, bases, inst2type


def main(rm_txt, cmsis_h, out_json):
    rows, arrays, deferred = parse_rm(rm_txt)
    periph, bases, inst2type = parse_cmsis(cmsis_h)

    #
    # ⭐ THE REFERENCE MANUAL CONTRADICTS ITSELF, AND THE JOIN CANNOT SEE IT.
    #
    # The same (name, offset) appears with DIFFERENT reset values in different
    # chapters -- because generic names repeat across peripherals.  On MCX N:
    #     VERID @0x000 -> FOURTEEN different values.  PARAM @0x004 -> twelve.
    # On rt1180: MP_CSR @0h -> 0031_0000h (DMA3) AND 0040_0000h (DMA4).
    #
    # Dropping rows that CMSIS attributes to >1 peripheral TYPE catches most of these
    # -- BY LUCK.  It does NOT catch the case where CMSIS IS UNAMBIGUOUS AND THE
    # MANUAL IS NOT, and there the tool would emit ONE OF THE VALUES ARBITRARILY AND
    # CALL IT A GOLDEN.
    #
    #     ⭐ A WRONG GOLDEN IS WORSE THAN A MISSING ONE.  It makes the CHECKER lie,
    #        and then your oracle is the thing that needs an oracle.
    #
    # That is this file's own rule, applied one level deeper than this file applied
    # it.  rt1180emulator found it porting the tool.  So: detect RM-side conflicts,
    # DROP THEM, and COUNT WHAT WE DROPPED.
    #
    seen = collections.defaultdict(set)
    for name, off, _w, _a, reset in rows:
        seen[(name, off)].add(reset)
    contradictory = {k for k, v in seen.items() if len(v) > 1}
    rm_conflicts = sum(1 for r in rows if (r[0], r[1]) in contradictory)
    rows = [r for r in rows if (r[0], r[1]) not in contradictory]

    owner = collections.defaultdict(set)
    for t, regs in periph.items():
        for n, offs in regs.items():
            for o in offs:
                owner[(n, o)].add(t)

    #
    # ⭐ THE RM IS AUTHORITATIVE FOR RESET VALUES.  CMSIS IS AUTHORITATIVE FOR
    #    ADDRESSES.  USE EACH SOURCE FOR WHAT IT ACTUALLY KNOWS.
    #
    # Joining on (name, offset) assumes the RM's offset column is always relative to
    # the PERIPHERAL base.  IT IS NOT.  The eDMA chapter numbers the CHANNEL SUB-BLOCK
    # FROM ZERO -- its row for CH0_CSR..CH15_CSR literally reads "0h - F000h" -- while
    # CMSIS places CH_CSR at 0x1000.  Same register, same manual, two different bases.
    #
    # The offset join therefore produced NO MATCH and SILENTLY DROPPED THE ENTIRE eDMA
    # CHANNEL BLOCK: the very block in which this project has found more silent-wrong-
    # answer bugs than any other.  THE GATE THAT EXISTS TO CATCH THAT CLASS COULD NOT
    # LOOK AT IT.  (It never produced a WRONG golden -- a mismatched offset matches
    # nothing -- but an invisible register is not a checked one.)
    #
    # So: try (name, offset) first, and fall back to NAME ALONE under conditions strict
    # enough that a wrong join is impossible:
    #     * the name must map to EXACTLY ONE CMSIS peripheral type, and
    #     * the RM must give that name EXACTLY ONE reset value anywhere in the manual.
    # The ADDRESS then comes from CMSIS, which is the thing CMSIS is for.
    #
    # For the name-only fallback we need the name to be UNAMBIGUOUS: exactly one type
    # AND exactly one offset.  A name with two offsets cannot be resolved without one.
    by_name = collections.defaultdict(set)
    for t, regs in periph.items():
        for n, offs in regs.items():
            if len(offs) == 1:
                by_name[n].add(t)
    rm_reset_by_name = collections.defaultdict(set)
    for name, _o, _w, _a, reset in rows:
        rm_reset_by_name[name].add(reset)

    #
    # ⭐ WIDTH.  DO NOT KEEP ONLY THE 32-BIT REGISTERS.
    #
    # This filter used to be `width != 32 -> skip`, and it threw away 340 rows: 250
    # sixteen-bit and 90 eight-bit.  Among them THE ENTIRE eDMA TCD BLOCK -- TCD_CSR,
    # TCD_CITER, TCD_BITER, TCD_SOFF, TCD_DOFF, TCD_ATTR -- which is the HEART of the DMA
    # engine and the block in which this project has found more silent-wrong-answer bugs
    # than any other.  THE GATE HAD NEVER ONCE LOOKED AT IT.
    #
    # rt1180emulator did this on his tree and the refusal pile contained the MOTOR DRIVE:
    # eFlexPWM DTCNT0/DTCNT1, the DEAD-TIME counters, reset 0x07FF on silicon and ZERO in
    # his model.  ZERO DEAD TIME IS A DIRECT SHORT ACROSS THE DC BUS THROUGH BOTH
    # TRANSISTORS OF AN INVERTER LEG.  Every PWM test green.  All of them 32-bit-blind.
    #
    #     ⭐ THE DANGEROUS ZEROS ARE THE ONES WHERE ZERO IS A LEGAL, MEANINGFUL,
    #        CATASTROPHIC VALUE -- not the ones where it is merely wrong.
    #
    golden, unmatched, ambiguous, by_name_joins = [], 0, 0, 0
    for name, off, width, acc, reset in rows:
        if width not in (8, 16, 32) or acc not in ("RW", "RO", "R"):
            continue

        types = owner.get((name, off))
        cmsis_off = off
        if not types:
            # Fall back to name-only, under the strict conditions above.
            cand = by_name.get(name)
            if (cand and len(cand) == 1
                    and len(rm_reset_by_name[name]) == 1):
                types = cand
                t0 = next(iter(cand))
                cmsis_off = next(iter(periph[t0][name]))   # CMSIS owns the ADDRESS
                by_name_joins += 1
            else:
                unmatched += 1
                continue
        if len(types) > 1:
            ambiguous += 1          # DROP, never guess -- and count what you dropped
            continue

        t = next(iter(types))
        for inst, ity in inst2type.items():
            if ity == t:
                golden.append({"inst": inst, "reg": name,
                               "addr": bases[inst] + cmsis_off, "reset": reset,
                               "width": width})

    #
    # ⚠ DEDUPE.  The RM gives some peripherals a chapter PER INSTANCE (DMA0 and DMA1 each
    # get their own), so the SAME register is emitted once per chapter -- and the join
    # then fans each row out across every instance, producing the register TWICE per
    # instance.  168 duplicates, all in agreement, but they inflate the CENSUS (making a
    # block look twice as covered as it is) and they double the question stream, which is
    # the thing that just pushed us past the pipe buffer.
    #
    # They AGREE here.  If they ever disagree, that is an RM contradiction and the check
    # above has already dropped it -- so a surviving duplicate is safe to collapse.
    #
    seen = {}
    for g in golden:
        seen[(g["inst"], g["reg"])] = g
    golden = list(seen.values())

    golden.sort(key=lambda x: (x["inst"], x["addr"]))
    json.dump(golden, open(out_json, "w"), indent=0)

    print("RM rows parsed            : %d  (%d array ranges expanded)"
          % (len(rows) + rm_conflicts, arrays))
    print("  RM SELF-CONTRADICTORY   : %d rows across %d (name,offset) keys -- DROPPED."
          % (rm_conflicts, len(contradictory)))
    print("                            (the manual gives the SAME register two different")
    print("                             reset values.  A WRONG golden makes the CHECKER")
    print("                             lie, so we refuse to pick one.)")
    print("  unmatched in CMSIS      : %d   (RM names a register CMSIS does not, there)" % unmatched)
    print("  ambiguous (>1 periph)   : %d   (DROPPED, not guessed)" % ambiguous)
    print("  joined by NAME (RM used a sub-block offset base): %d" % by_name_joins)
    print("golden                    : %d registers, %d instances -> %s"
          % (len(golden), len({g['inst'] for g in golden}), out_json))
    print()
    if deferred:
        names = sorted({n for n, _c in deferred})
        print()
        print("⚠ DEFERRED BY THE MANUAL : %d rows (%d distinct registers) whose reset cell"
              % (len(deferred), len(names)))
        print("  says 'See section' instead of a number -- USUALLY BECAUSE THE VALUE IS")
        print("  PER-INSTANCE, which is exactly the shape that was hiding the PORT PCR pads")
        print("  (PORT0 = 0x1143 -- THE SWD DEBUG PINS).  These are NOT in the golden and")
        print("  NOTHING CHECKS THEM.  A refusal is not a check, and an UNCOUNTED refusal")
        print("  is not even a refusal.  Triage them by hand:")
        print("    %s" % ", ".join(names[:16]))
        if len(names) > 16:
            print("    ... and %d more" % (len(names) - 16))
        with open(out_json.replace(".json", "-deferred.txt"), "w") as f:
            f.write("# Rows the MANUAL declines to answer ('See section').\n"
                    "# NOT in the golden.  NOTHING CHECKS THEM.  Triage by hand.\n")
            for n in names:
                f.write("%s\n" % n)
        print("  -> %s" % out_json.replace(".json", "-deferred.txt"))
    print()
    print("⚠ COVERAGE IS A FLOOR, NOT A CEILING.  Rows whose table layout did not")
    print("  parse are INVISIBLE to every check built on this file.  Validate against")
    print("  hand-read anchors before you trust a single byte of it: a parser is a")
    print("  model of a document, and a model that is its own oracle passes every test.")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
