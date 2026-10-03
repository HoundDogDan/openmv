# Build E — audio, IMU, and display removal

Incremental over the working Build D source for OPENMV_N6_OOK. The corrected
reference for all prior builds is approximately 122 mA during the CNN test.
Build E has not yet been compiled or measured on the device.

## Changes

- Set MICROPY_PY_AUDIO, MICROPY_PY_IMU, MICROPY_PY_DISPLAY and MICROPY_PY_TV
  to zero. Forward each explicit zero to both compiler flags and MicroPython
  make arguments. Other boards retain their existing settings.
- Remove automatic IMU initialization. In this source revision, STM32 main.c
  calls py_imu_init at startup when enabled; that function sets the LSM6DSM
  accelerometer and gyro to 52 Hz. The IMU drivers are omitted from the build.
- Omit the microphone interface configuration and dedicated IC8/ADF1 clock
  configuration. During board early initialization and standby exit, disable
  and clear the ADF1 interrupt, hold ADF1 in reset, disable its bus and sleep
  clocks, and disable IC8. The PDM filter library is excluded by AUDIO=0.
- Remove native display/TV support and the frozen display.py, ssd1306.py,
  and bno055.py helpers.

Camera, GenX320, CSI, ulab, pay_cnn, pay_rlif, USB/IDE/REPL, CPU frequency,
shared PLLs, image algorithms, ML/NPU, GPU/video support and memory layout
retain the Build D configuration. In particular, PLL3 remains enabled for
other consumers. No display pins or shared GPIO/DMA controllers are shut
down; G13 is also exposed as P7 and remains available to application code.

Audio and display normally initialize through application use, whereas IMU
initialization is automatic. Removing these modules does not establish a
particular current saving. This is a grouped experiment; any observed change
cannot be attributed to one member without a subsequent isolated test.

## Apply and build

Save the patch outside build/, which clean-dev deletes. Preserve the working
D firmware and source before cleaning. From the repository root:

```sh
git apply --check /path/to/OPENMV_N6_OOK_build_E.patch
git apply /path/to/OPENMV_N6_OOK_build_E.patch
cd docker
make clean-dev
set -o pipefail
make build-firmware-dev TARGET=OPENMV_N6_OOK V=1 2>&1 | tee ../ook-build-e.log
```

Keep the SDK_DIR and other options used for D. clean-dev also removes the
shared mpy-cross build. Firmware output is in build/OPENMV_N6_OOK/bin/.
Use the same firmware image type and flashing procedure as D.

## Power-cycle, check, then measure

After flashing, completely remove USB and external power, then reconnect.
An MCU reset alone can leave the externally powered IMU running with its
previous configuration. Build E removes the driver's startup; it does not
send an I2C shutdown command to an already-running sensor.

Run verify_build_e.py in the IDE. Expected unavailable modules:
network, socket, ssl, bluetooth, audio, imu, display and udisplay. CSI,
ulab and both classifier info calls must still work. TV is part of the
display implementation in this source revision, not a separate module.
A module stored separately on the device filesystem can obscure import
checks; inspect such files if results differ. These checks establish module
availability, not the actual IMU/clock register states or classifier accuracy.

Run the unchanged CNN workload with the same 30-second measurement interval,
supply voltage, USB state, warmup, camera settings, lighting and logging as D.
Record mean and maximum current, decoding success, and throughput. Do not
leave the verification script running during the measured interval.

| Configuration | CNN mean current | Status |
| --- | --- | --- |
| Prior builds, including D | Approximately 122 mA | User-corrected measurements; D module checks passed |
| E: audio, IMU and display removed | Pending | Compile, flash, power-cycle and measure |

## Preparation validation

GNU Make evaluation confirmed explicit zero compiler/submake flags for the
four E features and the previous networking removals. CSI, ulab, protocol and
ML flags remain enabled. Driver selection omits the IMU sources and retains
GenX320. The original OPENMV_N6 target still enables the four features.
Board-header preprocessing verifies removal of the microphone configuration
and IC8 settings. All new clock/reset APIs exist in the pinned STM32N6 HAL/LL
headers. Python syntax and forward/reverse patch application were checked.

The preparation workspace lacks the OpenMV SDK; no full ARM compile/link or
hardware measurement was performed here. The Docker build and device test
remain necessary.
