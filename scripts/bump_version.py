from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")

VALID_LEVELS = ("major", "minor", "patch")

TRUTHY = {"1", "true", "yes", "on"}


def highest_tag() -> tuple[int, int, int]:
    result = subprocess.run(
        ["git", "tag", "--list"],
        check=True,
        capture_output=True,
        text=True,
    )
    versions = []
    for line in result.stdout.splitlines():
        m = TAG_RE.match(line.strip())
        if m:
            versions.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    if not versions:
        return (0, 0, 0)
    return max(versions)


def bump(version: tuple[int, int, int], level: str) -> tuple[int, int, int]:
    major, minor, patch = version
    if level == "major":
        return (major + 1, 0, 0)
    if level == "minor":
        return (major, minor + 1, 0)
    return (major, minor, patch + 1)


def tag_exists(tag: str) -> bool:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}"],
        capture_output=True,
    )
    return result.returncode == 0


def do_bump(level: str, execute: bool, remote: str) -> int:
    if level not in VALID_LEVELS:
        print(
            f"error: invalid level '{level}', expected one of {VALID_LEVELS}",
            file=sys.stderr,
        )
        return 2

    current = highest_tag()
    new_version = bump(current, level)
    new_tag = f"v{new_version[0]}.{new_version[1]}.{new_version[2]}"

    if tag_exists(new_tag):
        print(f"error: tag {new_tag} already exists", file=sys.stderr)
        return 1

    tag_cmd = ["git", "tag", new_tag]
    push_cmd = ["git", "push", remote, new_tag]

    if execute:
        print(" ".join(tag_cmd))
        subprocess.run(tag_cmd, check=True)
        print(" ".join(push_cmd))
        subprocess.run(push_cmd, check=True)
    else:
        print(f"git tag {new_tag} && git push {remote} {new_tag}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Bump semver git tag.")
    parser.add_argument(
        "level",
        nargs="?",
        default="patch",
        choices=list(VALID_LEVELS),
    )
    parser.add_argument("--remote", default="origin")
    parser.add_argument(
        "--execute",
        action="store_true",
        help="Run git tag and git push instead of printing.",
    )
    args = parser.parse_args()
    return do_bump(args.level, args.execute, args.remote)


def _register_pio_target() -> None:
    try:
        from SCons.Script import DefaultEnvironment  # type: ignore[import-untyped]
    except ImportError:
        return

    env = DefaultEnvironment()

    def _run(execute_default: bool):
        def action(target, source, env):
            level = os.environ.get("BUMP_LEVEL", "patch").strip().lower()
            env_execute = os.environ.get("BUMP_EXECUTE", "").strip().lower()
            execute = execute_default or env_execute in TRUTHY
            remote = os.environ.get("BUMP_REMOTE", "origin").strip() or "origin"
            rc = do_bump(level, execute, remote)
            if rc != 0:
                raise SystemExit(rc)

        return action

    env.AddCustomTarget(
        name="bump_version",
        dependencies=None,
        actions=[_run(execute_default=False)],
        title="Bump semver git tag (dry-run)",
        description=(
            "Print next semver tag command without touching git. "
            "BUMP_LEVEL=major|minor|patch (default patch), "
            "BUMP_REMOTE=origin. Set BUMP_EXECUTE=1 or use "
            "bump_version_execute target to actually tag and push."
        ),
    )

    env.AddCustomTarget(
        name="bump_version_execute",
        dependencies=None,
        actions=[_run(execute_default=True)],
        title="Bump semver git tag (tag + push)",
        description=(
            "Bump patch/minor/major git tag and push to remote. "
            "BUMP_LEVEL=major|minor|patch (default patch), "
            "BUMP_REMOTE=origin."
        ),
    )


if __name__ == "__main__":
    sys.exit(main())
else:
    _register_pio_target()
