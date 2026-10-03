# OPENMV_N6_OOK — Build B control

This document describes the original Build B control. For the current Build C
changes and measured baseline, see [POWER_BASELINE.md](POWER_BASELINE.md).

This target copies the feature configuration of Dan Scott's Build A, before any
power-related removals. It is intended to establish that isolating the board
target preserves the live GenX320 OOK workload.

Baseline sources:

- Repository: https://github.com/HoundDogDan/openmv
- Branch: BareBones
- OpenMV commit: d682af03f6805408593a3f703c77fadc8a40e2a9
- Pinned MicroPython commit: cf80cce8a0ea98683be2cf40b1cf2b0c74a111a8
- Required OpenMV SDK: 1.6.0

## Changes

The eight OpenMV board files are copied from `boards/OPENMV_N6`. The nine
MicroPython board files are copied from the pinned submodule into this target's
`micropython/` directory. No submodule modifications are required.

The top-level Makefile accepts an optional `OMV_MPY_BOARD_DIR` from a board
configuration and uses it for both OpenMV's include paths and MicroPython's
`BOARD_DIR`. Existing targets keep their original routing.

This target defines `OPENMV_N6` in addition to `OPENMV_N6_OOK` to preserve the
existing N6-specific conditional in `modules/py_helper.c`. The copied
MicroPython linker configuration uses `$(BOARD_DIR)/board.ld`.

Feature settings, board names, USB VID/PID, CPU frequency, memory layout, frozen
modules, and ROMFS contents are preserved. The shared `pay_cnn` and `pay_rlif`
sources and headers are untouched. This is functional configuration parity,
not a claim that the resulting binaries will be byte-identical.

## Apply and build

Start in the repository root of your working Build A checkout. Preserve any
existing local changes first; do not replace your working environment with a
fresh SDK or different submodule revisions for this comparison.

```sh
git switch -c ook-build-b
git apply --check /path/to/OPENMV_N6_OOK_build_B.patch
git apply /path/to/OPENMV_N6_OOK_build_B.patch
make TARGET=OPENMV_N6_OOK -j$(nproc)
```

Retain any additional flags from your successful Build A command. The command
above is for a Bash/Linux build environment. Expected firmware output is under
`build/OPENMV_N6_OOK/bin/`, including `firmware.bin` and `openmv.bin`. Use the same
flashing procedure and appropriate image type as for Build A.

Use `make TARGET=OPENMV_N6` to build the original target. Each target has a
separate build directory.

## Measurement

Run the same script, classifier, payload, camera settings, lighting, clock
settings, USB connection state, supply voltage, and measurement interval as
Build A. Compare mean current, decoded frame count, and classification results.
Keep CNN and SNN measurements separate. Repeat runs to assess normal variation.

Do not assign the illustrative 120 mA from the plan as a measured result.

| Configuration | Supply voltage | Mean current | Actual interval | Completed frames | Accuracy |
| --- | --- | --- | --- | --- | --- |
| A: OPENMV_N6 at d682af0 | | | | | |
| B: OPENMV_N6_OOK, same features | | | | | |

The current `main_v4.py` disables per-packet output but still prints progress
every five seconds. Keep that behavior consistent for this comparison.

## Findings for subsequent removal stages

`board_config.mk` enables CYW43, Bluetooth/NimBLE, lwIP, SSL/mbedTLS, audio,
IMU, display/TV, FIR/ToF, ML/STAI, and VC8000. `board_config.h` also enables
GPU/Nema and JPEG/video support. Enabled software does not itself establish
that its peripheral is active or consuming a particular amount of current.

`common/micropy.mk` forwards several feature flags only when they equal 1.
The copied `micropython/mpconfigboard.mk` defaults Bluetooth, NimBLE, lwIP,
CYW43, SSL, and mbedTLS to 1. Therefore a later removal patch must ensure that
explicit zero settings reach the MicroPython build as well; changing only an
OpenMV flag can leave MicroPython enabled. Frozen dependencies such as aioble
must be reviewed in the same removal group.

Ethernet pin/PHY configuration is in `micropython/mpconfigboard.h`; Ethernet
driver compilation is guarded by the presence of `MICROPY_HW_ETH_MDC` in the
pinned STM32 port. The Ethernet removal stage must also trace clock and PHY
behavior. It is not part of this control build.

The custom classifiers use C inference implementations and do not directly
call STAI/NPU APIs. Preserve `pay_cnn`, `pay_rlif`, `ulab`, and the GenX320/CSI
path when later removing the general ML framework.

## Validation status

All copied board files were compared with the pinned sources. Only the target
routing, the N6 compatibility define, the local linker path, and removal of
two inherited trailing spaces differ.
The MicroPython submodule remains unmodified, and patch whitespace checks pass.

A build attempt stopped at the missing OpenMV SDK check in the preparation
workspace. Compilation, linking, boot, decoding, and power equivalence remain
to be verified in the working build environment and on the board.
