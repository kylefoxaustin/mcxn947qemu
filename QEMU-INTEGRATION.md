# QEMU build integration

New source files to drop in, plus additions to existing QEMU build files
(Kconfig / meson.build). Local to this repo.

## New files -> placement in the QEMU tree

```
include/hw/arm/mcxn_soc.h       ->  include/hw/arm/
include/hw/char/mcxn_lpuart.h   ->  include/hw/char/
include/hw/misc/mcxn_scg.h      ->  include/hw/misc/
hw/arm/mcxn_soc.c               ->  hw/arm/
hw/arm/mcxn_frdm.c              ->  hw/arm/
hw/char/mcxn_lpuart.c           ->  hw/char/
hw/misc/mcxn_scg.c              ->  hw/misc/
```

## hw/arm/Kconfig — append

```kconfig
config MCXN_SOC
    bool
    depends on TCG && ARM
    select ARMV7M
    select UNIMP
    select MCXN_LPUART
    select MCXN_SCG

config FRDM_MCXN947
    bool
    default y
    depends on TCG && ARM
    select MCXN_SOC
```

## hw/char/Kconfig — append

```kconfig
config MCXN_LPUART
    bool
```

## hw/misc/Kconfig — append

```kconfig
config MCXN_SCG
    bool
```

## hw/arm/meson.build — add next to the existing i.MX entries

```meson
arm_common_ss.add(when: 'CONFIG_MCXN_SOC',      if_true: files('mcxn_soc.c'))
arm_common_ss.add(when: 'CONFIG_FRDM_MCXN947',  if_true: files('mcxn_frdm.c'))
```

## hw/char/meson.build — add

```meson
system_ss.add(when: 'CONFIG_MCXN_LPUART', if_true: files('mcxn_lpuart.c'))
```

## hw/misc/meson.build — add

```meson
system_ss.add(when: 'CONFIG_MCXN_SCG', if_true: files('mcxn_scg.c'))
```
