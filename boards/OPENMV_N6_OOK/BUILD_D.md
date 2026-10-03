# Build D — Ethernet and network-stack removal

Build D is incremental over the working Build C source, including the
submodules-recipe and board-object archive fixes. The firmware target remains
OPENMV_N6_OOK. A Git branch named ook-build-d does not change the target name.

## Changes

- Disable lwIP, the network and socket modules, and SSL/mbedTLS. Explicit zero
  values reach both OpenMV C compilation and MicroPython's sub-make, overriding
  the copied board's default-on settings.
- Omit the board's Ethernet pin/PHY definitions. In the pinned STM32 port,
  eth.c, eth_phy.c and network_lan.c guard their implementations with
  MICROPY_HW_ETH_MDC, so this removes the MAC/PHY driver and LAN binding.
- In board early initialization and after standby, disable/clear the ETH1
  interrupt, hold the MCU Ethernet block in reset, disable its MAC/TX/RX/bus
  clocks and their sleep enables, and disable IC12. Remove the unused IC12
  clock configuration. Shared PLLs, CPU frequency, and camera clocks stay as C.
- Remove socket/TLS-dependent frozen helpers: ssl, ntptime, webrepl, rtsp,
  mqtt, requests, and microdot-lib. Preserve the independent RPC helper
  and the separate native OpenMV USB/IDE protocol.

Wi-Fi/Bluetooth remain disabled. USB/REPL, GenX320/CSI, ulab, pay_cnn, pay_rlif,
audio, IMU, image/ML support, GPU, memory pools, and ROMFS models retain their
Build C configuration. This stage measures the remaining networking group,
including TLS; it does not isolate lwIP from TLS individually.

## PHY boundary

The source config names an RTL8211 PHY and RGMII pins but defines no physical
PHY power/reset GPIO. Build D removes PHY driver initialization and shuts down
the STM32 MAC/clocks. It does not claim to remove supply power from an external
PHY or place such a PHY into its MDIO power-down mode. An attached Ethernet
board would need its actual wiring and power/reset controls identified for
that separate step. No unidentified pins are driven by this patch.

The original driver initializes the MAC/PHY in response to a LAN object; it
does not prove that hardware was active in the CNN baseline. lwIP startup and
its periodic poll dispatch are independently removed by MICROPY_PY_LWIP=0.

## Apply and build

Preserve the working C revision and its firmware before cleaning. Save the
patch outside build/, because the Docker clean-dev target deletes that tree.
From the repository root:

```sh
git switch -c ook-build-d
git apply --check /path/to/OPENMV_N6_OOK_build_D.patch
git apply /path/to/OPENMV_N6_OOK_build_D.patch
cd docker
make clean-dev
set -o pipefail
make build-firmware-dev TARGET=OPENMV_N6_OOK V=1 2>&1 | tee ../ook-build-d.log
```

Keep any additional SDK_DIR or Docker options from the successful C build.
clean-dev also removes the shared mpy-cross build directory. Cleaning is
recommended for this feature change to eliminate objects from disabled stacks.

Firmware output is under build/OPENMV_N6_OOK/bin/. Flash using the same image
type and procedure as C, then power-cycle.

## Check and measure

Run verify_build_d.py in the OpenMV IDE before the measurement. It expects
network, socket, ssl, and bluetooth imports to be unavailable and verifies that
csi, ulab, pay_cnn and pay_rlif remain importable. A module copied separately
to the device filesystem can make an import check ambiguous; inspect that
case instead of interpreting it as proof of a native module being enabled.

The module check establishes availability, not classifier accuracy or measured
clock-register state. Run the same CNN script and 30-second interval used in C.
Keep payload, camera biases, ROI/watch pixels, lighting, supply voltage, USB
connection state, warmup, logging and CPU frequency the same. Record mean and
maximum current, decoding success, completed frames, and accuracy.

| Build | Workload | Mean current | Result |
| --- | --- | --- | --- |
| Baseline/control | CNN | Approximately 122 mA | User-corrected reference |
| C: Wi-Fi/Bluetooth removed | CNN | Approximately 122 mA | No observed reduction; runtime module checks passed |
| D: remaining networking removed | CNN, 30 seconds | Pending | Awaiting Docker build and device measurement |

The earlier 121.59-to-118.18 mA comparison and derived 2.80% savings are
superseded by the user's corrected approximately 122 mA observations.

## Validation

Checked explicit compiler and sub-make flags for all eight disabled network
settings, while preserving CSI, ulab, native USB protocol, and STAI settings.
The actual MicroPython submodules dry run excludes lwIP, mbedTLS, CYW43 and
NimBLE for D; the original OPENMV_N6 target retains them. Verified that the
Ethernet pin guard is absent and that every clock/reset API used exists in
the pinned STM32N6 HAL/LL headers. The patch was checked against the saved C
source and the Python runtime-check script passes syntax validation.

No full ARM compile/link or hardware run was performed in the preparation
workspace, which lacks the OpenMV SDK. The successful C build log is evidence
for C only. D remains unverified on the device until the new build is tested.
