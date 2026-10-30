"""Makes the copy of an APK that can be published: the libraries keep the absolute paths of the
source files they were built from (__FILE__ in logs and asserts), which name this PC's user and
folders. Each path's prefix is overwritten with as many underscores, so nothing else in the file
moves, and the APK is aligned and signed again with the same debug key as tools/build-apk.sh.

usage: py -3 tools/scrub_apk.py <input.apk> <output.apk>
"""
import os
import re
import subprocess
import sys
import tempfile
import zipfile

PREFIX = re.compile(rb"[A-Z]:/Users/[^\0]*?/(?=gta4-360-port/)")
BUILD_TOOLS = os.path.join(os.environ["LOCALAPPDATA"], "Android", "Sdk", "build-tools", "36.0.0")
KEYSTORE = os.path.join(os.path.expanduser("~"), ".android", "debug.keystore")
USER = os.path.basename(os.path.expanduser("~")).encode()


def scrub(data):
    return PREFIX.sub(lambda match: b"_" * (len(match.group(0)) - 1) + b"/", data)


def personal(data):
    lowered = data.lower()
    return [term for term in (USER.lower(), USER.lower().decode().encode("utf-16-le"), b"users/", b"users\\")
            if term in lowered]


source, output = sys.argv[1], sys.argv[2]
os.makedirs(os.path.dirname(output), exist_ok=True)
with tempfile.TemporaryDirectory() as folder:
    unsigned, aligned = os.path.join(folder, "unsigned.apk"), os.path.join(folder, "aligned.apk")
    with zipfile.ZipFile(source) as apk, zipfile.ZipFile(unsigned, "w") as copy:
        for info in apk.infolist():
            # The old signature goes: it covers the old bytes.
            if info.filename.startswith("META-INF/"):
                continue
            data = apk.read(info)
            clean = scrub(data)
            assert len(clean) == len(data), info.filename
            left = personal(clean)
            if data != clean or left:
                changed = sum(a != b for a, b in zip(data, clean))
                print(f"{info.filename}: {len(PREFIX.findall(data))} paths, {changed} bytes changed, left {left}")
            assert not left, info.filename
            entry = zipfile.ZipInfo(info.filename, info.date_time)
            entry.compress_type = info.compress_type
            copy.writestr(entry, clean)
    subprocess.run([os.path.join(BUILD_TOOLS, "zipalign.exe"), "-f", "-p", "4", unsigned, aligned], check=True)
    subprocess.run([os.path.join(BUILD_TOOLS, "apksigner.bat"), "sign", "--ks", KEYSTORE, "--ks-pass", "pass:android",
                    "--key-pass", "pass:android", "--out", output, aligned], check=True)
print(output, os.path.getsize(output))
