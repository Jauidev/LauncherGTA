#!/usr/bin/env bash
# Version of the app: MAJOR.MINOR.PATCH plus a "D" suffix on development builds (e.g. 0.2.0D).
#   major  big change (milestone, architecture)      -> resets MINOR and PATCH
#   minor  medium change (feature, subsystem rework)  -> resets PATCH
#   patch  small change (bug fix, tweak, setting)
# The versionCode follows the name: MAJOR*10000 + MINOR*100 + PATCH, so every bump installs over
# the previous build. Without an argument it prints the current version.
# Two version lines: the Mali build (android/AndroidManifest.xml, gta4port.apk) and, with snap
# first, the Snapdragon build (android-snap/AndroidManifest.xml, gta4snap.apk, from 0.1.0D).
# Usage: bash tools/bump-version.sh [snap] [major|minor|patch]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="$ROOT/android/AndroidManifest.xml"
if [ "${1:-}" = snap ]; then
  shift
  MANIFEST="$ROOT/android-snap/AndroidManifest.xml"
fi

name=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$MANIFEST")
if [[ ! "$name" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(D?)$ ]]; then
  echo "bump-version: versionName '$name' is not MAJOR.MINOR.PATCH[D]" >&2
  exit 1
fi
major=${BASH_REMATCH[1]} minor=${BASH_REMATCH[2]} patch=${BASH_REMATCH[3]} suffix=${BASH_REMATCH[4]}

case "${1:-}" in
  "") echo "$name"; exit 0 ;;
  major) major=$((major + 1)); minor=0; patch=0 ;;
  minor) minor=$((minor + 1)); patch=0 ;;
  patch) patch=$((patch + 1)) ;;
  *) echo "usage: bash tools/bump-version.sh [snap] [major|minor|patch]" >&2; exit 1 ;;
esac
if ((minor > 99 || patch > 99)); then
  echo "bump-version: MINOR and PATCH must stay under 100 (versionCode)" >&2
  exit 1
fi

new="$major.$minor.$patch$suffix"
code=$((major * 10000 + minor * 100 + patch))
sed -i "s/android:versionName=\"[^\"]*\"/android:versionName=\"$new\"/; s/android:versionCode=\"[0-9]*\"/android:versionCode=\"$code\"/" "$MANIFEST"
echo "$name -> $new (versionCode $code)"
