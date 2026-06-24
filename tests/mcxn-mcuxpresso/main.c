/*
 * Minimal MCUXpresso SDK app for QEMU frdm-mcxn947.
 *
 * Exercises the stock NXP SDK driver path end to end against the QEMU model:
 *   startup_MCXN947_cm33_core0.S -> SystemInit -> BOARD_BootClockFRO12M
 *   (fsl_clock) -> BOARD_InitDebugConsole (LP_FLEXCOMM_Init + LPUART_Init via
 *   fsl_lpflexcomm/fsl_lpuart) -> PRINTF (debug_console_lite + fsl_str).
 *
 * A successful run proves the model satisfies the NXP SDK's LP_FLEXCOMM
 * present-bit gating (PSELID.UARTPRESENT) and LPUART baud/CTRL programming —
 * the second, independent BSP firmware path alongside Zephyr.
 *
 * Build with build.sh against a local open mcux-sdk checkout (see README.md).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "board.h"
#include "clock_config.h"
#include "fsl_debug_console.h"

int main(void)
{
    BOARD_BootClockFRO12M();      /* fsl_clock: bring up the 12 MHz FRO */
    BOARD_InitDebugConsole();     /* attach FlexComm4 clock + init LPUART4 console */

    PRINTF("Hello from the MCUXpresso SDK on QEMU frdm-mcxn947!\r\n");
    PRINTF("SDK path OK: startup + fsl_clock + fsl_lpflexcomm/fsl_lpuart + debug console.\r\n");
    PRINTF("MCUXPRESSO-SDK-PASS\r\n");

    while (1) {
    }
}
