#!/usr/bin/env python3
"""
Post-build script to copy firmware.bin to bin/ directory
"""
# PlatformIO uses SCons Import function
Import("env")  # noqa: F821
import shutil
from pathlib import Path

def copy_firmware(source, target, env):
    """Copy built firmware to bin/ directory"""
    # Get firmware path from build
    firmware_path = str(target[0])
    
    # Create bin directory in project root
    project_dir = Path(env['PROJECT_DIR'])
    bin_dir = project_dir / 'bin'
    bin_dir.mkdir(exist_ok=True)
    
    # Copy firmware
    dest = bin_dir / 'firmware.bin'
    shutil.copy2(firmware_path, dest)
    
    print(f"✅ Firmware copied to: {dest}")

# Register post-build action
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)  # noqa: F821
