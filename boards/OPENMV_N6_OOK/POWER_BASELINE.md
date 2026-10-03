# OPENMV_N6_OOK power measurements

## Recorded results

| Build | Configuration | Workload | Interval | Mean current | Validation |
| --- | --- | --- | --- | --- | --- |
| A | BareBones, original OPENMV_N6 at d682af0 | — | — | Not recorded here | Working source baseline |
| B | OPENMV_N6_OOK, original feature settings | CNN | 30 seconds | 122 mA | User reports built, flashed, and still working |
| C | Build B minus CYW43 Wi-Fi and Bluetooth/NimBLE; both radio enables low | CNN | Use 30 seconds | Pending | Source/configuration checks only |

Build B is the measured reference for this sequence. Supply voltage, repeat-run
variation, frame counts, and numeric accuracy were not supplied with its result.
The 120 mA in the original plan was illustrative, not a measured Build A result.

## Build C changes

- Set MICROPY_PY_NETWORK_CYW43, MICROPY_PY_BLUETOOTH, and
  MICROPY_BLUETOOTH_NIMBLE to zero in the OOK board configuration.
- Forward explicit zero settings through common/micropy.mk to the C compiler
  and the MicroPython sub-make. This overrides the copied MicroPython board's
  default-on settings without changing the original N6 target.
- Remove aioble from the effective OpenMV frozen-module manifest.
- In the OOK board's early initialization, hold WL_REG_ON (PB12) and BT_REG_ON
  (PD10) low when both radio features are disabled. Pin names come from this
  board's pins.csv; the pinned CYW43 source uses these active-high enables.

lwIP, Ethernet, SSL, USB/REPL, CPU clocks, GenX320/CSI, ulab, pay_cnn, pay_rlif,
and other feature groups retain their Build B settings. The MicroPython
submodule is not edited. Runtime WLAN and Bluetooth support are absent in C.

The pinned cyw43_init() already drives WL_REG_ON low before activation. A small
or unmeasurable current reduction is therefore a valid outcome for an OOK
script that never activates Wi-Fi. Do not equate linked driver size with active
radio current. Power-rail leakage and total board power still require measurement.

## Apply after Build B

Run in the working OpenMV repository root, with the Build B patch already
applied. If Build B has uncommitted changes, commit the relevant Build B source
files first so the known-working state remains recoverable.

```sh
git switch -c ook-build-c
git apply --check /path/to/OPENMV_N6_OOK_build_C.patch
git apply /path/to/OPENMV_N6_OOK_build_C.patch
make TARGET=OPENMV_N6_OOK clean
make TARGET=OPENMV_N6_OOK -j$(nproc)
```

Keep all additional flags used for Build B. Clean and build are separate
commands; do not run the clean target in parallel with the build.
Firmware output remains in build/OPENMV_N6_OOK/bin/. Use the same flash
procedure and image type as Build B, then power-cycle the board.

## Verify before measuring

Run these checks in the REPL before starting the measured interval:

```python
import csi
import pay_cnn
import pay_rlif
from ulab import numpy as np
import network

assert not hasattr(network, "WLAN")
try:
    import bluetooth
except ImportError:
    print("Bluetooth disabled")
else:
    raise RuntimeError("Unexpected Bluetooth module")
print("CNN:", pay_cnn.info())
print("SNN:", pay_rlif.info())
```

Do not run these checks during the measured workload. Rerun the same CNN script
used for Build B, with the same payload, camera settings, lighting, supply,
USB connection state, warmup, logging, and 30-second interval. Confirm that
decoding still works and record the completed frame count and accuracy as well
as mean current.

Current reduction relative to B is 122 mA minus the new mean. Percentage
reduction is 100 times that difference divided by 122 mA. A negative reduction
means current increased. Repeat close results before interpreting a small delta.

## Validation limits

Configuration checks exercise the real GNU Make feature-forwarding logic and
the copied MicroPython defaults. Patch application is checked against the
saved Build B source. This preparation workspace lacks OpenMV SDK 1.6.0;
compilation, linking, boot, runtime imports, and power savings are not verified
here. Build C's result remains pending until the device run is reported.
