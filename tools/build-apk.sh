#!/usr/bin/env bash
# Builds build/apk/gta4port.apk without Gradle: javac -> d8 -> aapt2 link -> add classes.dex and
# lib/arm64-v8a/libgta4.so (and libgta4iv.so, GTA IV) -> zipalign -> apksigner (Android debug keystore).
# Run tools/build-android.sh gta4app and tools/build-android.sh iv gta4app first.
# Install: adb install -r build/apk/gta4port.apk
# Usage: tools/build-apk.sh [snap]
#   (none)  gta4port.apk (android/AndroidManifest.xml, build/arm64): since 0.7.0D the one APK for
#           every phone. Its launcher tells Mali from Adreno; on Adreno, Mesa's Turnip
#           (third_party/turnip, as libvulkan_turnip.so) and libadrenotools' hooks, which
#           runtime/os/vk_driver.cpp loads, run the renderer when the phone's driver cannot.
#   snap    gta4snap.apk (android-snap/AndroidManifest.xml, build/arm64-snap, run
#           tools/build-android.sh snap gta4app first): the same app with the CPU tuned for the
#           Snapdragon 680, for comparisons. Same package and data folder, its own version line.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -W)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
BT="$SDK/build-tools/36.0.0"
JAR="$SDK/platforms/android-34/android.jar"
JDK="${JAVA_HOME:-C:/Program Files/Android/Android Studio/jbr}"
STRIP="$SDK/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
VARIANT="${1:-port}"
case "$VARIANT" in
  port) NAME=gta4port; OUT="$ROOT/build/apk"; MANIFEST="$ROOT/android/AndroidManifest.xml"
        BUILD="$ROOT/build/arm64"; BUILD_IV="$ROOT/build/arm64-iv" ;;
  snap) NAME=gta4snap; OUT="$ROOT/build/apk-snap"; MANIFEST="$ROOT/android-snap/AndroidManifest.xml"
        BUILD="$ROOT/build/arm64-snap"; BUILD_IV="" ;;
  *) echo "usage: tools/build-apk.sh [snap]" >&2; exit 1 ;;
esac
TURNIP="$ROOT/third_party/turnip/v26.0.0-R8/vulkan.ad07xx.so"
HOOKS="$BUILD/third_party/libadrenotools/src/hook"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/lib/arm64-v8a"

"$JDK/bin/javac" -source 8 -target 8 -bootclasspath "$JAR" -nowarn -Xlint:-options \
    -d "$OUT/classes" $(find "$ROOT/android/src" -name '*.java')
"$BT/d8.bat" --release --min-api 29 --lib "$JAR" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
# The app icon (LAUNCHER/res, made by tools/make_icon.py).
"$BT/aapt2.exe" compile --dir "$ROOT/LAUNCHER/res" -o "$OUT/res.zip"
"$BT/aapt2.exe" link -o "$OUT/base.apk" --manifest "$MANIFEST" -I "$JAR" "$OUT/res.zip" \
    --min-sdk-version 29 --target-sdk-version 34

# The APK carries a stripped copy; $BUILD/libgta4.so keeps the symbols for llvm-symbolizer
# (crash offsets are the same in both).
"$STRIP" --strip-debug -o "$OUT/lib/arm64-v8a/libgta4.so" "$BUILD/libgta4.so"

# Debug Vulkan layers (the app is debuggable, so the Vulkan loader finds them in the app's library
# directory). VALIDATION=1: Khronos validation, enabled with GTA4_XENIA_CVARS=vulkan_validation=true.
# GFXR=1: LunarG GFXReconstruct, enabled with adb shell setprop debug.vulkan.layers
# VK_LAYER_LUNARG_gfxreconstruct (captures are configured with the debug.gfxrecon.* properties).
LAYERS=()
# GTA IV's library, loaded instead of libgta4.so when that game is chosen (MainActivity).
if [ -n "$BUILD_IV" ]; then
  "$STRIP" --strip-debug -o "$OUT/lib/arm64-v8a/libgta4iv.so" "$BUILD_IV/libgta4.so"
  LAYERS+=("$OUT/lib/arm64-v8a/libgta4iv.so")
fi
if [ "${VALIDATION:-0}" = 1 ]; then
  LAYERS+=("$ROOT/third_party/vulkan-validation/arm64-v8a/libVkLayer_khronos_validation.so")
fi
if [ "${GFXR:-0}" = 1 ]; then
  LAYERS+=("$ROOT/third_party/gfxreconstruct/arm64-v8a/libVkLayer_gfxreconstruct.so")
fi
# Turnip and adrenotools' hooks go in the same folder (the package manager extracts it to
# nativeLibraryDir, where adrenotools looks for both); only Adreno phones load them.
cp "$TURNIP" "$OUT/lib/arm64-v8a/libvulkan_turnip.so"
LAYERS+=("$OUT/lib/arm64-v8a/libvulkan_turnip.so")
for hook in hook_impl main_hook file_redirect_hook gsl_alloc_hook; do
  "$STRIP" --strip-debug -o "$OUT/lib/arm64-v8a/lib$hook.so" "$HOOKS/lib$hook.so"
  LAYERS+=("$OUT/lib/arm64-v8a/lib$hook.so")
done

# LibertyRecomp's font atlases (2048x2048; the game's fonts are 512x512), stored uncompressed so
# MainActivity can read their sizes (AssetManager.openFd) and copy them to the app's files folder.
FONTS="$ROOT/third_party/rexglue/LibertyRecompLib/font_atlases"
# The launcher's backgrounds (LAUNCHER/: Mali blue, Adreno red) and its font
# (runtime/os/launcher_ui.cpp reads them with the AssetManager).
LAUNCHER="$ROOT/LAUNCHER"
UI_FONT="$ROOT/third_party/xenia-gpu/third_party/imgui/misc/fonts/Roboto-Medium.ttf"

py -3 - "$OUT" "$FONTS" "$LAUNCHER" "$UI_FONT" "${LAYERS[@]}" <<'EOF'
import sys, zipfile, os
out, fonts, launcher, ui_font, layers = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
with zipfile.ZipFile(os.path.join(out, "base.apk"), "a", zipfile.ZIP_DEFLATED) as apk:
    apk.write(os.path.join(out, "dex", "classes.dex"), "classes.dex")
    apk.write(os.path.join(out, "lib", "arm64-v8a", "libgta4.so"), "lib/arm64-v8a/libgta4.so")
    for layer in layers:
        apk.write(layer, "lib/arm64-v8a/" + os.path.basename(layer))
    for episode in ("", "tlad/", "tbogt/"):
        for name in ("font1.png", "font2.png", "font3.png"):
            apk.write(os.path.join(fonts, episode + name), "assets/font_atlases/" + episode + name,
                      zipfile.ZIP_STORED)
    apk.write(os.path.join(launcher, "FONDO DE MALI.png"), "assets/launcher/bg_mali.png", zipfile.ZIP_STORED)
    apk.write(os.path.join(launcher, "FONDO DE ADRENO.png"), "assets/launcher/bg_adreno.png", zipfile.ZIP_STORED)
    apk.write(ui_font, "assets/launcher/Roboto-Medium.ttf")
EOF

"$BT/zipalign.exe" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner.bat" sign --ks "$USERPROFILE/.android/debug.keystore" --ks-pass pass:android \
    --key-pass pass:android --out "$OUT/$NAME.apk" "$OUT/aligned.apk"
ls -la "$OUT/$NAME.apk"

# Keep a copy named by its version (tools/bump-version.sh sets it before a build with changes).
VERSION=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$MANIFEST")
cp "$OUT/$NAME.apk" "$ROOT/build/$NAME-$VERSION.apk"
echo "version $VERSION -> build/$NAME-$VERSION.apk"
