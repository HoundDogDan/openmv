# OpenMV library
add_library("openmv-lib", "$(OMV_LIB_DIR)")

# Drivers
# Build E: frozen bno055, ssd1306 and display helpers are omitted.
freeze ("$(OMV_LIB_DIR)/", "modbus.py")
freeze ("$(OMV_LIB_DIR)/", "pid.py")
freeze ("$(OMV_LIB_DIR)/", "tb6612.py")
freeze ("$(OMV_LIB_DIR)/", "vl53l1x.py")
freeze ("$(OMV_LIB_DIR)/", "machine.py")

# Build C: aioble is omitted with the Bluetooth firmware support.

# Build D: socket/TLS-dependent network helpers are omitted.
# The OpenMV USB/IDE protocol below remains enabled.
# Preserve the independent RPC helper.
freeze ("$(OMV_LIB_DIR)/", "rpc.py")

# Utils
require("time")
require("logging")
require("collections-defaultdict")
require("types")
require("senml")

# Libraries
require("ml", library="openmv-lib")
require("protocol", library="openmv-lib")
include("$(MPY_DIR)/extmod/asyncio")

# Boot script
freeze ("$(OMV_LIB_DIR)/", "_boot.py")
