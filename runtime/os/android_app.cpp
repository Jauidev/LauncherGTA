// gta4port: JNI entry points of libgta4.so for the Android app (android/src/com/gta4port/
// MainActivity.java). The game runs headless on its own thread; the activity shows
// os::status::Describe() as text until the renderer exists.
#if defined(__ANDROID__) && defined(GTA4_SHARED)
#include <stdafx.h>
#include <jni.h>
#include <pthread.h>
#include <android/native_window_jni.h>
#include "status.h"
#include "touch_pad.h"
#include "perf_hint.h"
#include "swap_in.h"
#include "background.h"
#include <kernel/memory.h>

int Gta4Main();
void SetGpuSurface(ANativeWindow* window); // runtime/gpu/command_processor.cpp

namespace
{
    std::string JavaString(JNIEnv* env, jstring value)
    {
        if (value == nullptr)
            return {};
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string result = chars;
        env->ReleaseStringUTFChars(value, chars);
        return result;
    }

    void* GameThread(void*)
    {
        Gta4Main();
        LOGN("game thread finished");
        return nullptr;
    }
}

// options: extra "NAME=value" pairs separated by spaces (GTA4_AUTOPRESS=1, GTA4_LOG_FILES=1...),
// passed with: adb shell am start -n com.gta4port/.MainActivity --es env "GTA4_AUTOPRESS=1"
extern "C" JNIEXPORT void JNICALL Java_com_gta4port_MainActivity_nativeStart(JNIEnv* env, jclass, jstring gameDir, jstring saveDir, jstring options)
{
    static bool s_started;
    if (s_started)
        return;
    s_started = true;

    setenv("GTA4_GAME_DIR", JavaString(env, gameDir).c_str(), 1);
    setenv("GTA4_SAVE_DIR", JavaString(env, saveDir).c_str(), 1);
    // ReXGlue's user folder (native profiler exports, SPIR-V dumps) is $XDG_DATA_HOME, else
    // $HOME/.local/share = /data/.local/share, which the app cannot write: the profile was lost
    // without a message. Use the app's external files folder, next to save/ (readable with adb).
    setenv("XDG_DATA_HOME", std::filesystem::path(JavaString(env, saveDir)).parent_path().c_str(), 1);
    // LibertyRecomp native Vulkan renderer (rexglue_glue/ + third_party/rexglue)
    setenv("GTA4_GPU", "native", 1);
    setenv("GTA4_RES", "960x540", 1);
    setenv("GTA4_AFFINITY", "1", 1);
    setenv("GTA4_NATIVE_CVARS",
           // No sun shadows (the user's choice, 2026-10-27): the shadow pass was half of all draws.
           // Interval 0 drops its draws and leaves the map cleared; one cascade instead of 5 also
           // spares the game building the rest. files/shadows = 1 brings them back (live, near
           // shadows only), files/shadow_cascades = 0 the stock 5 cascades (at the next start).
           "gta4_native_shadow_interval=0,"
           "gta4_shadow_cascade_count=1,"
           // Sun-shadow draws smaller than a box (under 12 triangles: litter, cards, decals) are
           // skipped: each costs the recorder as much as a building. files/shadow_min = 0 restores.
           "gta4_native_shadow_min_indices=36,"
           // Sun shadows (2026-10-27): the console's range instead of twice it, and a 960x960 map
           // instead of 1280x1280. The shadow pass was half of all draws (~1700 of ~3800 per frame);
           // half the range leaves out the far casters, and each map texel covers half the ground,
           // so near shadows end up 1.5x sharper than before even with 0.75x the texels.
           // files/shadow_range = 2.0 and files/shadow_size = 256 restore (at the next start).
           "gta4_shadow_map_base_size=192,"
           "gta4_shadow_distance_scale=1.0,"
           "gta4_native_merge_up_max=64,"
           "gta4_native_merge_indexed_draws=true,"
           "gta4_draw_distance_scale=1.0,"
           // Fog at 700 m instead of the far city at 1500 (the user's request, 2026-10-27): the far
           // clip is capped there and the game's own fog closes in to hide the cut (fog.cpp).
           // files/fog = 0 restores the game's distance, another number moves it (live).
           "gta4_fog_distance=700,"
           "gta4_native_water_reflection_interval=2,"
           "gta4_native_environment_reflection_interval=4,"
           // The fps guard runs inside the dynamic draw distance (on Android, gated on the recorder).
           // Its level 0 set both reflections to every frame and levels 1-3 the environment one to
           // every other frame: always more recorder and GPU work than the intervals above.
           // files/fps_guard = 3 restores it.
           "gta4_fps_guard=0,"
           "gta4_native_water_reflection_min_indices=100,"
           "gta4_native_early_fragment_tests=false,"
           "gta4_native_fs_relaxed_precision=false,"
           // Light setup draws keep the scope's color attachments: 45-55 fewer scope breaks per
           // frame, each a LOAD+STORE of whole targets. Turned off after white squares on distant
           // coronas, seen on Turnip (Adreno), not on the Mali. files/reuse_lighting = B turns it off.
           "gta4_native_reuse_lighting_real=true,"
           "gta4_native_lazy_resolves=true,"
           "gta4_native_resolve_swap=true,"
           "gta4_native_gpu_detile=true,"
           "gta4_native_pipelined_recording=true,"
           "gta4_native_async_command_retire=true,"
           // Reflections (water, mirrors, environment) with 1 sample instead of the 4 the game asks
           // for: the scene is already 1 sample + FXAA, and the 4-sample captures were the only
           // multisampled targets left (stored whole to memory on the Mali).
           "gta4_reflection_aa=off", 1);
    // Direct resolves (color, 2x2 box, float24 depth): same image, less GPU work on the Mali.
    setenv("GTA4_XENIA_DIRECT_RESOLVE", "7", 1);
    // Render targets only as tall as GTA IV uses (2048 rows of samples, the shadow map is 1280)
    // instead of all of the EDRAM: up to ~150 MB less.
    setenv("GTA4_XENIA_RT_MAX_SAMPLE_ROWS", "2048", 1);
    // RAM (the phone has 7.5 GB and the game used ~2.4): textures unused for 10 s are freed once
    // the texture cache passes 128 MB, all old ones past 384 MB (Xenia: 30 s, 384 and 768 MB).
    // Shadow maps at standard resolution (sun 1280x1280, S = 256).
    setenv("GTA4_SHADOW_SIZE", "256", 1);
    // Every texture in its own memory (shared blocks were left ~40% empty by freed textures).
    setenv("GTA4_XENIA_TEXTURE_DEDICATED", "1", 1);
    setenv("GTA4_XENIA_CVARS_DEFAULTS",
           "texture_cache_memory_limit_soft=128,texture_cache_memory_limit_soft_lifetime=10,"
           "texture_cache_memory_limit_hard=384", 1);

    const std::string list = JavaString(env, options);
    size_t start = 0;
    while (start < list.size())
    {
        size_t end = list.find(' ', start);
        if (end == std::string::npos)
            end = list.size();
        const std::string pair = list.substr(start, end - start);
        const size_t equals = pair.find('=');
        if (equals != std::string::npos)
            setenv(pair.substr(0, equals).c_str(), pair.substr(equals + 1).c_str(), 1);
        start = end + 1;
    }
    // The launcher's graphics choices that have no files/ switch (reflection intervals and
    // antialiasing, os/launcher_ui.cpp) go after the defaults above, so they win; a
    // GTA4_NATIVE_CVARS in the options replaces the defaults and still gets them.
    if (const char* launcher = getenv("GTA4_LAUNCHER_CVARS"); launcher && *launcher)
    {
        const char* cvars = getenv("GTA4_NATIVE_CVARS");
        const std::string merged = cvars && *cvars ? std::string(cvars) + "," + launcher : std::string(launcher);
        setenv("GTA4_NATIVE_CVARS", merged.c_str(), 1);
    }

    // Recompiled functions use large host stack frames; the console's main thread is deep too.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 64 * 1024 * 1024);
    pthread_t thread;
    StartSwapInThread();
    pthread_create(&thread, &attributes, GameThread, nullptr);
    pthread_attr_destroy(&attributes);
    pthread_detach(thread);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_gta4port_MainActivity_nativeStatus(JNIEnv* env, jclass)
{
    return env->NewStringUTF(os::status::Describe().c_str());
}

// The frame counter over the game (MainActivity): {frames per second, longest frame in ms} since
// the previous call.
extern "C" JNIEXPORT jfloatArray JNICALL Java_com_gta4port_MainActivity_nativeFrameCounter(JNIEnv* env, jclass)
{
    float values[2];
    FrameCounterRead(values[0], values[1]);
    jfloatArray result = env->NewFloatArray(2);
    if (result)
        env->SetFloatArrayRegion(result, 0, 2, values);
    return result;
}

// The SurfaceView's surface for the GPU (created, resized, or null when destroyed). The GPU is
// detached from the old window before it is released.
extern "C" JNIEXPORT void JNICALL Java_com_gta4port_MainActivity_nativeSetSurface(JNIEnv* env, jclass, jobject surface)
{
    static ANativeWindow* s_window;
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (window && window == s_window)
    {
        ANativeWindow_release(window); // same window resized: keep the reference already held
        window = s_window;
    }
    SetGpuSurface(window);
    if (s_window && s_window != window)
        ANativeWindow_release(s_window);
    s_window = window;
}
// The touch controls' virtual pad (android/src/com/gta4port/TouchControls.java), set on every
// change: XAMINPUT_GAMEPAD_* buttons, triggers 0..255, sticks -32768..32767 (Y up).
extern "C" JNIEXPORT void JNICALL Java_com_gta4port_MainActivity_nativeSetPad(JNIEnv*, jclass, jint buttons, jint leftTrigger,
    jint rightTrigger, jint thumbLX, jint thumbLY, jint thumbRX, jint thumbRY)
{
    os::touch_pad::State state;
    state.buttons = uint16_t(buttons);
    state.leftTrigger = uint8_t(std::clamp(leftTrigger, 0, 255));
    state.rightTrigger = uint8_t(std::clamp(rightTrigger, 0, 255));
    state.thumbLX = int16_t(std::clamp(thumbLX, -32768, 32767));
    state.thumbLY = int16_t(std::clamp(thumbLY, -32768, 32767));
    state.thumbRX = int16_t(std::clamp(thumbRX, -32768, 32767));
    state.thumbRY = int16_t(std::clamp(thumbRY, -32768, 32767));
    os::touch_pad::Set(state);
}
// 0x82C30C14 is the flag the IS_PAUSE_MENU_ACTIVE script native (sub_82602368) returns.
static bool PauseMenuOpen()
{
    return *reinterpret_cast<volatile uint8_t*>(g_memory.base + 0x82C30C14) != 0;
}

// The activity's window lost or regained input focus: the notification shade came down, the
// recent apps opened, the app went to the background. GTA pauses only from its own pause menu
// (it ignores the console Guide's XN_SYS_UI, tried 2026-10-27), so a focus loss presses START for
// 200 ms unless that menu is already open. The game resumes from the menu, as after a START.
extern "C" JNIEXPORT void JNICALL Java_com_gta4port_MainActivity_nativeSetFocus(JNIEnv*, jclass, jboolean focus)
{
    if (focus || !g_memory.base)
        return;
    const bool menu = PauseMenuOpen();
    LOGFN("focus lost, pause menu {}", menu ? "already open" : "opened (START)");
    if (!menu)
        os::touch_pad::Press(XAMINPUT_GAMEPAD_START, 200);
}

// The activity stopped (the home screen or another app covers it) or started again. Stopped, the
// game is held (os/background.h) once the pause menu that the focus loss asked for is open, or
// after 1.5 s without it (loading screens): the START press has to reach the game first.
extern "C" JNIEXPORT void JNICALL Java_com_gta4port_MainActivity_nativeSetBackground(JNIEnv*, jclass, jboolean background)
{
    if (!background || !g_memory.base)
    {
        ReleaseBackgroundHold();
        return;
    }
    const uint32_t ticket = RequestBackgroundHold();
    std::thread([ticket]
    {
        for (int waited = 0; waited < 1500 && !PauseMenuOpen(); waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EngageBackgroundHold(ticket);
    }).detach();
}
#endif
