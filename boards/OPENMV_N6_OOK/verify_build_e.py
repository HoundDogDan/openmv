"""Run in OpenMV before, not during, the 30-second CNN measurement."""

for name in ("network", "socket", "ssl", "bluetooth", "audio", "imu", "display", "udisplay"):
    try:
        __import__(name)
    except ImportError:
        print(name + ": disabled")
    else:
        raise RuntimeError(name + " is still importable; check firmware and filesystem modules")

import csi
import pay_cnn
import pay_rlif
from ulab import numpy as np

print("CSI and ulab available")
print("CNN:", pay_cnn.info())
print("SNN:", pay_rlif.info())
print("Build E module checks passed; run the unchanged CNN power test next.")
