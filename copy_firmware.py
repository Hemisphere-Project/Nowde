#!/usr/bin/env python3
"""
Post-build script: copy the built firmware to dist/firmware-<env>.bin.
The DevKit env also keeps the historical firmware.bin name, which is what the
MillluBridge OTA uploader has always pointed at.

dist/ is gitignored on purpose. bin/ holds the RELEASE binaries and changes only
in a release commit (README, "Build and flash"): a build that wrote there let any
`pio run` followed by a broad `git add` republish untested bytes (cbd43bd).
"""
# PlatformIO uses SCons Import function
Import("env")  # noqa: F821
import shutil
from pathlib import Path

def copy_firmware(source, target, env):
    firmware_path = str(target[0])
    project_dir = Path(env['PROJECT_DIR'])
    dist_dir = project_dir / 'dist'
    dist_dir.mkdir(exist_ok=True)

    envname = env['PIOENV']
    dest = dist_dir / f'firmware-{envname}.bin'
    shutil.copy2(firmware_path, dest)
    print(f"✅ Firmware copied to: {dest}")

    if envname == 'esp32-s3-devkitc-1':
        legacy = dist_dir / 'firmware.bin'
        shutil.copy2(firmware_path, legacy)
        print(f"✅ Firmware copied to: {legacy} (legacy name)")

# Register post-build action
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)  # noqa: F821
