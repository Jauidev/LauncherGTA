"""Copies what the public repository holds (LauncherGTA) into another folder. It is a list of what
to publish, not of what to leave out, so anything new stays private until it is added here: the
game's files, the recompiled code (ppc/, ppc_iv/), its compiled shaders, saves, logs and notes never
get there. Also writes patches/ with the changes to the unmodified third-party clones.

usage: py -3 -I tools/export_public.py <repository folder>     (run from the project root)
"""
import os, shutil, subprocess, sys

FILES = ["CMakeLists.txt", "build-xenonrecomp.bat"]
FOLDERS = ["runtime", "rexglue_glue", "xenia_glue", "android", "android-snap", "cmake", "config", "tools",
           "third_party/libmspack",
           "third_party/rexglue/src", "third_party/rexglue/include", "third_party/rexglue/gta4-recomp",
           "third_party/rexglue/thirdparty/picosha2", "third_party/rexglue/thirdparty/renderdoc"]
THIRD_PARTY_FILES = ["third_party/rexglue/LICENSE", "third_party/rexglue/COPYING-LibertyRecomp"]
ANALYSIS_FOLDERS = ["analysis", "analysis/gta4"]
ANALYSIS_EXTENSIONS = (".tsv", ".toml", ".patch")
SKIP_NAMES = {"__pycache__", ".git", "export_chat.py", "shader_override_cache.cpp"}
SKIP_EXTENSIONS = (".pyc", ".log", ".err", ".iso", ".xex", ".apk", ".so", ".exe", ".dll", ".pdb")
# Clones with the port's changes: origin, and the paths whose changes are ours.
PATCHED = {"xenia-gpu": ["."], "XenonRecomp": ["XenonAnalyse", "XenonRecomp", "XenonUtils"]}

output = os.path.abspath(sys.argv[1])


def copy(path):
    target = os.path.join(output, path)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.copyfile(path, target)


def git(folder, *arguments):
    return subprocess.run(["git", "-C", folder, "-c", "core.safecrlf=false", *arguments],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout


for path in FILES + THIRD_PARTY_FILES:
    copy(path)
for folder in FOLDERS:
    for root, names, files in os.walk(folder):
        names[:] = [name for name in names if name not in SKIP_NAMES]
        for name in files:
            if name not in SKIP_NAMES and not name.endswith(SKIP_EXTENSIONS):
                copy(os.path.join(root, name))
for folder in ANALYSIS_FOLDERS:
    for name in os.listdir(folder):
        if name.endswith(ANALYSIS_EXTENSIONS):
            copy(os.path.join(folder, name))

os.makedirs(os.path.join(output, "patches"), exist_ok=True)
for name, paths in PATCHED.items():
    folder = os.path.join("third_party", name)
    patch = git(folder, "diff", "--binary", "--ignore-submodules=all", "--", *paths)
    untracked = git(folder, "ls-files", "--others", "--exclude-standard", "--", *paths).decode().split("\n")
    for path in filter(None, untracked):
        patch += git(folder, "diff", "--binary", "--no-index", "--", "/dev/null", path)
    with open(os.path.join(output, "patches", name + ".patch"), "wb") as file:
        file.write(patch)
