NXP FRDM-MCXN947 (``frdm-mcxn947``)
===================================

The ``frdm-mcxn947`` machine models the NXP FRDM-MCXN947 development board,
which is built around the NXP MCXN947 SoC.  The MCXN947 is a dual-core
Arm Cortex-M33 microcontroller (running at up to 150 MHz) from the MCX N
series, with an FPU, DSP extensions, an MPU and Arm TrustZone-M
(SAU-based security).  It has 2 MiB of on-chip flash, 512 KiB of SRAM and
a 96 KiB SRAMX region.

The console is on LPUART4 (FlexComm4).

Supported devices
"""""""""""""""""

The ``frdm-mcxn947`` machine supports the following devices:

- Dual Arm Cortex-M33 cores (the second core is released by the SYSCON
  CPU-boot control, as on hardware)
- Nested Vectored Interrupt Controller (NVIC) and SysTick
- SCG, SYSCON and SPC (a derived clock tree: the core, bus, timers and
  peripheral clocks follow the configured PLL/divider sources)
- eDMA (DMA0/DMA1) with peripheral request lines
- FlexComm serial engines: LPUART, LPSPI (drives a real SSI device) and
  LPI2C (drives a real I2C device)
- FlexIO configured as an SPI master
- Timers: CTIMER, MRT, LPTMR, OSTIMER
- GPIO, PORT, PINT (pin interrupts) and INPUTMUX (trigger routing)
- FlexPWM/eFlexPWM, SCT and the QDC quadrature decoder
- Analog blocks driven by operator inputs (QOM properties): ADC, DAC,
  HSCMP, TSI and the SINC filter
- Audio: SAI and the PDM/MICFIL microphone interface
- FlexSPI (including execute-in-place boot from an external NOR flash)
- FlexCAN, ENET-QoS Ethernet and uSDHC (drives a real ``sd-card``)
- USB full-speed and high-speed controllers (device and host modes)
- FMU (flash program/erase), the ELS EdgeLock secure sub-system (with a
  per-boot-seeded PRNG), PUF and other security blocks
- PowerQuad DSP coprocessor, and the Mailbox / SEMA42 blocks used for
  dual-core RPMsg/OpenAMP messaging

Missing devices
"""""""""""""""

The following blocks are present as register interfaces but their compute
or data path is intentionally **not** modelled, because it depends on
information that is not publicly documented or on a peer that has no model
upstream.  Rather than return a fabricated result, each fails honestly (it
does not silently report a completed operation over untouched data):

- SmartDMA (the EZH programmable core runs proprietary firmware and has no
  documented instruction set) - the register protocol is modelled and a
  boot is decoded and named, but no program is executed
- eIQ Neutron NPU (proprietary microcode) - the uncomputed result is
  flagged to the guest via the error-trap interrupt
- EMVSIM smartcard interface (no ISO-7816 card model upstream)

Boot options
""""""""""""

The machine loads a firmware image with the ``-kernel`` option:

.. code-block:: bash

  $ qemu-system-arm -M frdm-mcxn947 -kernel firmware.elf

Execute-in-place boot from an external FlexSPI NOR flash (with no internal
flash image) is selected with the ``qspi-boot`` machine option.  The image
is loaded into the FlexSPI XIP region and run in place; the boot ROM's
FlexSPI Configuration Block gate is enforced, so an image without a valid
config block is rejected:

.. code-block:: bash

  $ qemu-system-arm -M frdm-mcxn947,qspi-boot=on -kernel firmware.elf
