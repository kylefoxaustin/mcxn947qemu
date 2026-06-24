#!/usr/bin/env bash
#
# Build the MCUXpresso SDK hello_world for frdm-mcxn947 from a *local* open
# (BSD-3-Clause) mcux-sdk checkout.  The SDK sources are NOT redistributed in
# this repo — point MCUX_SDK / CMSIS_DIR at your own checkout (see README.md).
#
# Output: mcux-hello.elf in this directory (also git-ignored — do not commit).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
S="${MCUX_SDK:-$HOME/mcux-sdk}"
CMSIS="${CMSIS_DIR:-$HOME/CMSIS_5/CMSIS/Core/Include}"
CC="${CC:-arm-none-eabi-gcc}"
OUT="${OUT:-$HERE/mcux-hello.elf}"

[ -d "$S/devices/MCXN947" ] || { echo "ERROR: mcux-sdk not at \$MCUX_SDK ($S)"; exit 1; }
[ -d "$CMSIS" ]            || { echo "ERROR: CMSIS Core not at \$CMSIS_DIR ($CMSIS)"; exit 1; }

INC="-I$S/devices/MCXN947 -I$S/devices/MCXN947/drivers \
-I$S/drivers/common -I$S/drivers/lpflexcomm -I$S/drivers/lpflexcomm/lpuart \
-I$S/boards/frdmmcxn947 -I$S/utilities/debug_console_lite -I$S/utilities/str \
-I$S/drivers/gpio -I$S/drivers/mcx_spc -I$S/components/uart -I$CMSIS"

SRC="$S/devices/MCXN947/gcc/startup_MCXN947_cm33_core0.S \
$S/devices/MCXN947/system_MCXN947_cm33_core0.c \
$S/devices/MCXN947/drivers/fsl_clock.c \
$S/devices/MCXN947/drivers/fsl_reset.c \
$S/drivers/gpio/fsl_gpio.c \
$S/drivers/mcx_spc/fsl_spc.c \
$S/components/uart/fsl_adapter_lpuart.c \
$S/drivers/common/fsl_common.c \
$S/drivers/common/fsl_common_arm.c \
$S/drivers/lpflexcomm/fsl_lpflexcomm.c \
$S/drivers/lpflexcomm/lpuart/fsl_lpuart.c \
$S/boards/frdmmcxn947/board.c \
$S/boards/frdmmcxn947/clock_config.c \
$S/utilities/debug_console_lite/fsl_debug_console.c \
$S/utilities/str/fsl_str.c \
$HERE/main.c"

"$CC" -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -DCPU_MCXN947VDF_cm33_core0 -DNDEBUG -Os -ffreestanding -Wall \
  $INC $SRC \
  -T "$S/devices/MCXN947/gcc/MCXN947_cm33_core0_flash.ld" \
  -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections \
  -o "$OUT"
echo "built $OUT"
