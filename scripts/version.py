"""PlatformIO pre-script: inject release parameters as compiler defines.

FW_VERSION    semantic version baked into the image (default 0.0.0-dev)
BLINK_MS      LED half-period in ms, so each release is visibly different
SIMULATE_BAD  =1 builds an image that crashes before it validates itself,
              used to exercise the bootloader rollback path
"""
import os

Import("env")  # noqa: F821

version = os.environ.get("FW_VERSION", "0.0.0-dev")
blink_ms = os.environ.get("BLINK_MS", "1000")

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("FW_VERSION", env.StringifyMacro(version)),  # noqa: F821
        ("BLINK_MS", blink_ms),
    ]
)
if os.environ.get("SIMULATE_BAD") == "1":
    env.Append(CPPDEFINES=["SIMULATE_BAD_FW"])  # noqa: F821

print("== firmware version %s, blink %s ms%s" % (
    version, blink_ms, " [SIMULATED BAD IMAGE]" if os.environ.get("SIMULATE_BAD") == "1" else ""))
