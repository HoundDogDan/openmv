"""Run in OpenMV before, not during, the 30-second power measurement."""

for name in ("network", "socket", "ssl", "bluetooth"):
    try:
        __import__(name)
    except ImportError:
        print(name + ": disabled")
    else:
        raise RuntimeError(name + " is still importable; check the flashed image")

import csi
import pay_cnn
import pay_rlif
from ulab import numpy as np

print("CSI and ulab available")
print("CNN:", pay_cnn.info())
print("SNN:", pay_rlif.info())
print("Build D module checks passed; run the unchanged CNN power test next.")
