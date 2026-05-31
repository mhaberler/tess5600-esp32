# C++ preprocessor macros injected by this script (all are `const char*`
# string literals; guard with #ifdef before use):
#
#   BUILD_SHA            always, when in a git repo — `git rev-parse --short HEAD`
#   BUILD_DATE           always — UTC ISO-8601 build timestamp
#   BUILD_REPO           CI only — "$GITHUB_SERVER_URL/$GITHUB_REPOSITORY"
#   BUILD_TAG            CI tag builds — $GITHUB_REF_NAME (matches v?N.N.N…)
#   BUILD_FIRMWARE_URI   CI tag builds — release asset URL for the merged bin
#   SGO_DEFAULT_OWNER    if unset + git remote parseable — repo owner
#   SGO_DEFAULT_REPO     if unset + git remote parseable — repo name
#   SGO_DEFAULT_BIN      if unset — ota_bin_filename(env)
#
# Additional defines come from platformio.ini build_flags -
# CORE_DEBUG_LEVEL, AUTOCHECK_INTERVAL, USE_*, etc.).

from __future__ import annotations

import os
import re
import subprocess
from datetime import datetime, timezone
from typing import Any

from SCons.Script import DefaultEnvironment  # type: ignore[import-untyped]

from firmware_naming import (
    merged_bin_filename,
    ota_bin_filename,
    parse_github_remote,
)

env: Any = DefaultEnvironment()


def define_str(name, value):
    env.Append(CPPDEFINES=[(name, '\\"' + value + '\\"')])


def is_defined(name):
    for entry in env.get("CPPDEFINES", []):
        key = entry[0] if isinstance(entry, (tuple, list)) else entry
        if key == name:
            return True
    return False


def define_str_if_unset(name, value):
    if is_defined(name):
        return
    define_str(name, value)


def inject_sgo_defaults():
    project_dir = env.subst("$PROJECT_DIR")
    remote = parse_github_remote(project_dir)
    if remote:
        owner, repo = remote
        define_str_if_unset("SGO_DEFAULT_OWNER", owner)
        define_str_if_unset("SGO_DEFAULT_REPO", repo)
    define_str_if_unset("SGO_DEFAULT_BIN", ota_bin_filename(env))


def inject():
    # Git SHA — skip silently if not in a git repo
    try:
        sha = (
            subprocess.check_output(
                ["git", "rev-parse", "--short", "HEAD"],
                stderr=subprocess.DEVNULL,
            )
            .decode()
            .strip()
        )
        if sha:
            define_str("BUILD_SHA", sha)
    except Exception:
        pass

    # Build date — always available
    define_str("BUILD_DATE", datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))

    # CI-only: repo, tag, firmware URI
    github_repository = os.environ.get("GITHUB_REPOSITORY", "")
    github_server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    github_ref_name = os.environ.get("GITHUB_REF_NAME", "")

    if github_repository:
        define_str("BUILD_REPO", f"{github_server}/{github_repository}")

    tag_match = re.match(r"^v?(\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?)$", github_ref_name)
    if tag_match:
        tag = github_ref_name
        define_str("BUILD_TAG", tag)

        if github_repository:
            filename = merged_bin_filename(env)
            uri = f"{github_server}/{github_repository}/releases/download/{tag}/{filename}"
            define_str("BUILD_FIRMWARE_URI", uri)

    inject_sgo_defaults()


inject()
