from __future__ import annotations

import os
import re
import subprocess


def sanitize_filename_part(value):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", str(value)).strip("_")


def sanitize_project_name(value):
    return sanitize_filename_part(str(value).replace("-", "_"))


def normalize_version(value):
    value = str(value).strip()
    return value[1:] if value.startswith("v") else value


def _join_segments(*parts):
    return "_".join(p for p in parts if p)


def get_project_option(env, name, default=None):
    try:
        value = env.GetProjectOption(name)
    except Exception:
        return default
    if value is None:
        return default
    if isinstance(value, str):
        value = env.subst(value).strip().strip('"').strip("'")
        if value == "":
            return default
    return value


def read_version_from_file(path, pattern):
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(pattern, content)
    if not match:
        raise RuntimeError(f"Version is not found in {path}")
    return normalize_version(match.group(1))


def resolve_version(env):
    explicit_version = get_project_option(env, "custom_firmware_version")
    if explicit_version:
        return normalize_version(explicit_version)

    version_file = get_project_option(env, "custom_firmware_version_file")
    if version_file:
        pattern = get_project_option(
            env,
            "custom_firmware_version_regex",
            r"v?(\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?)",
        )
        project_dir = env.subst("$PROJECT_DIR")
        src_dir = env.subst("$PROJECT_SRC_DIR")
        candidates = []
        if os.path.isabs(version_file):
            candidates.append(version_file)
        else:
            candidates.append(os.path.join(src_dir, version_file))
            candidates.append(os.path.join(project_dir, version_file))
        for candidate in candidates:
            if os.path.exists(candidate):
                return read_version_from_file(candidate, pattern)
        raise RuntimeError(f"Version file is not found: {version_file}")

    github_ref_name = os.environ.get("GITHUB_REF_NAME", "")
    match = re.match(r"^v?(\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?)$", github_ref_name)
    if match:
        return normalize_version(match.group(1))

    return ""


def firmware_name(env):
    project_dir = env.subst("$PROJECT_DIR")
    name = get_project_option(env, "custom_firmware_name", os.path.basename(project_dir))
    return sanitize_project_name(name)


def firmware_env(env):
    pioenv = env.subst("$PIOENV")
    value = get_project_option(env, "custom_firmware_env", pioenv)
    return sanitize_filename_part(value)


def merged_bin_filename(env, suffix="bin"):
    suffix = sanitize_filename_part(get_project_option(env, "custom_firmware_suffix", suffix))
    version = sanitize_filename_part(resolve_version(env))
    base = _join_segments(firmware_name(env), firmware_env(env), "firmware", version)
    return f"{base}.{suffix}"


def ota_bin_filename(env, suffix="bin"):
    suffix = sanitize_filename_part(get_project_option(env, "custom_firmware_suffix", suffix))
    base = _join_segments(firmware_name(env), firmware_env(env), "ota")
    return f"{base}.{suffix}"


def parse_github_remote(cwd):
    try:
        url = subprocess.check_output(
            ["git", "-C", cwd, "remote", "get-url", "origin"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return None
    match = re.search(r"github\.com[:/]([^/]+)/(.+?)(?:\.git)?/?$", url)
    if not match:
        return None
    return match.group(1), match.group(2)
