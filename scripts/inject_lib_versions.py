import os, subprocess, re, shlex
Import("env")

# Library names come from `custom_inject_lib_versions` in the env section,
# whitespace-separated; quote names containing spaces.
raw = env.GetProjectOption("custom_inject_lib_versions", "")
libs = shlex.split(raw)

def manifest_version(name):
    for lb in env.GetLibBuilders():
        if lb.name == name:
            return (lb._manifest or {}).get("version")
    return None

def git_sha(name):
    path = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"),
                        env.subst("$PIOENV"), name)
    try:
        return subprocess.check_output(
            ["git", "-C", path, "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return None

def macro_name(name):
    s = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").upper()
    return f"{s}_VERSION"

defines = []
for name in libs:
    ver = manifest_version(name) or git_sha(name) or "unknown"
    defines.append((macro_name(name), env.StringifyMacro(ver)))
    print(f"inject_lib_versions: {macro_name(name)}={ver}")

env.Append(CPPDEFINES=defines)

# `projenv` is the env used to compile project src; only available after
# PlatformIO finishes assembling it.
def _inject_projenv(projenv):
    projenv.Append(CPPDEFINES=defines)

try:
    Import("projenv")
    _inject_projenv(projenv)
except Exception:
    env.AddPreAction("buildprog", lambda *a, **k: _inject_projenv(globals().get("projenv", env)))
