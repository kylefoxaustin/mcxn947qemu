# BSP-matrix boot-smoke (frdm-mcxn947)

The cheap, re-runnable regression that asserts the model still boots each
officially-supported firmware stack. Mirrors the fleet pattern (see the i.MX95
`tests/bsp-matrix/`), adapted to the MCX's axis.

**The MCX axis is different from the i.MX boards.** It's a bare-metal/RTOS
Cortex-M MCU — no NXP Linux BSP, no kernel/dtb/.wic/System-Manager. Its "BSP
versions" are the two officially-supported firmware stacks: **Zephyr** and the
**NXP MCUXpresso SDK**. The validated set is published in
[`docs/validation/bsp-matrix.yaml`](../../docs/validation/bsp-matrix.yaml) —
the model OWNS that list; downstream tools (holobench's version picker) READ its
`status:` (validated / candidate / blocked) rather than guessing a compat matrix.

## Run

```sh
tests/bsp-matrix/run.sh zephyr-v4.4            # Zephyr hello → "Hello World! frdm_mcxn947"
tests/bsp-matrix/run.sh mcuxpresso-sdk-2.16    # SDK hello   → "MCUXPRESSO-SDK-PASS"
ELF=/path/to.elf MARKER="..." tests/bsp-matrix/run.sh <label>   # any entry / canary
```

Boot is bare `-M frdm-mcxn947 -kernel <elf>` (no `-dtb`/`-initrd`/`-smp`).

## Redistribution

Zephyr ELFs are Apache-2.0 and staged at `~/mcxn-images/`. The MCUXpresso SDK
binary is **not** redistributable — operator-built via
[`tests/mcxn-mcuxpresso/build.sh`](../mcxn-mcuxpresso/) and kept out of the repo.
`run.sh` **skips cleanly** when an entry's artifact isn't staged.

## Deeper than boot-smoke

For the MCUXpresso entry, the boot-smoke is just the floor. The real depth is the
NXP **example corpus** (`~/mcux-ws`, harness `run-example.sh` / `batch-examples.sh`):
155 stock NXP examples built with NXP's own armgcc CMake and booted on the model.
Latest sweep: **113 PASS / 22 RAN / 13 BLDFAIL / 0 CPU faults** — snapshot in
[`docs/validation/mcuxpresso-corpus-scorecard.tsv`](../../docs/validation/mcuxpresso-corpus-scorecard.tsv).
