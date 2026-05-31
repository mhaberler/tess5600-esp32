from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
from typing import Any

from SCons.Script import DefaultEnvironment  # type: ignore[import-untyped]

from firmware_naming import (
    get_project_option as _gpo,
    merged_bin_filename,
    ota_bin_filename,
)

env: Any = DefaultEnvironment()


def get_project_option(name, default=None) -> Any:
    return _gpo(env, name, default)


def split_list(value):
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return list(value)
    return [item.strip() for item in re.split(r"[,\s]+", str(value)) if item.strip()]


def resolve_chip():
    chip = get_project_option("custom_firmware_chip")
    if chip:
        return chip.lower().replace("-", "")

    chip = get_project_option("board_build.mcu")
    if chip:
        return chip.lower().replace("-", "")

    board_config = env.BoardConfig()
    chip = board_config.get("build.mcu", None)
    if chip:
        return str(chip).lower().replace("-", "")

    raise RuntimeError(
        "Could not determine chip type. "
        "Please set custom_firmware_chip, for example: custom_firmware_chip = esp32s3"
    )


def generate_merged_firmware(target, source, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    progname = env.subst("$PROGNAME")

    firmware_dir = get_project_option("custom_firmware_dir", "firmware")
    if not os.path.isabs(firmware_dir):
        firmware_dir = os.path.join(project_dir, firmware_dir)
    os.makedirs(firmware_dir, exist_ok=True)

    firmware_suffix = get_project_option("custom_firmware_suffix", "bin")
    output_filename = merged_bin_filename(env, firmware_suffix)
    output_path = os.path.join(firmware_dir, output_filename)

    app_bin = os.path.join(build_dir, f"{progname}.{firmware_suffix}")

    if not os.path.exists(app_bin):
        raise RuntimeError(f"Application binary is not found: {app_bin}")

    extra_images = env.get("FLASH_EXTRA_IMAGES", [])
    if not extra_images:
        raise RuntimeError("FLASH_EXTRA_IMAGES is empty. Cannot build merged firmware.")

    chip = resolve_chip()

    cmd = [
        env.subst("$PYTHONEXE"),
        env.subst("$UPLOADER"),
        "--chip",
        chip,
        "merge-bin",
        "-o",
        output_path,
    ]

    flash_mode = get_project_option("custom_firmware_flash_mode")
    flash_freq = get_project_option("custom_firmware_flash_freq")
    flash_size = get_project_option("custom_firmware_flash_size")

    if flash_mode:
        cmd.extend(["--flash-mode", flash_mode])
    if flash_freq:
        cmd.extend(["--flash-freq", flash_freq])
    if flash_size:
        cmd.extend(["--flash-size", flash_size])

    for address, image in extra_images:
        cmd.extend([str(address), env.subst(image)])

    cmd.extend([env.subst("$ESP32_APP_OFFSET"), app_bin])

    print("Generating merged firmware:")
    print(" ".join(shlex.quote(part) for part in cmd))

    subprocess.check_call(cmd)

    print(f"Merged firmware generated: {output_path}")

    ota_filename = ota_bin_filename(env, firmware_suffix)
    ota_path = os.path.join(firmware_dir, ota_filename)
    shutil.copy2(app_bin, ota_path)
    print(f"OTA (app-only) binary copied: {ota_path}")


firmware_suffix = get_project_option("custom_firmware_suffix", "bin")
app_bin = os.path.join("$BUILD_DIR", f"${{PROGNAME}}.{firmware_suffix}")

dependencies = [app_bin]
dependencies.extend(split_list(get_project_option("custom_firmware_dependencies")))

env.AddCustomTarget(
    name="firmware",
    dependencies=dependencies,
    actions=[generate_merged_firmware],
    title="Generate merged firmware",
    description="Generate a single merged firmware image for distribution",
)
