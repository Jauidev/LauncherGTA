// gta4port: entry point of the runtime. Based on UnleashedRecomp's main.cpp (KiSystemStartup,
// LdrLoadModule, GuestThread::Start) without the installer, UI, mods or video device: phase 2
// runs headless until the game reaches its main loop.
#include <stdafx.h>
#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <os/paths.h>
#include <os/thread_dump.h>
#include <os/thread_policy.h>
#include "loader.h"

void InstallCrashHandler();

Memory g_memory;
Heap g_userHeap;

static void KiSystemStartup()
{
    g_userHeap.Init();

    const std::string gamePath = os::paths::GetGamePath().string();
    const auto gameContent = XamMakeContent(XCONTENTTYPE_RESERVED, "Game");
    XamRegisterContent(gameContent, gamePath);

    std::error_code ec;
    std::filesystem::create_directories(os::paths::GetSavePath(), ec);
    XamRegisterSaves();

    // The game reads its data through game:\ and d:\ (the disc).
    XamContentCreateEx(0, "game", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);
    XamContentCreateEx(0, "D", &gameContent, OPEN_EXISTING, nullptr, nullptr, 0, 0, nullptr);

    // update:\ holds the disc's own data patch (update\data\effects\entityFx.dat, ...); the game
    // never mounts it itself.
    XamRootCreate("update", (os::paths::GetGamePath() / "update").string());

    XAudioInitializeSystem();
}

// gta4port: GTA4_RES=<width>x<height> sets the game's render resolution. RAGE reads it from its
// "width"/"height" parameters, whose default values are the strings "1280" (0x82000A00, room for
// 7 characters) and "720" (0x820009FC, room for 3) in the XEX; sub_828C02E0 parses them at boot,
// so they are replaced right after the image is loaded. Height is limited to 3 digits.
static void ApplyRenderResolution()
{
    const char* setting = getenv("GTA4_RES");
    if (setting == nullptr)
        return;
    unsigned width = 0, height = 0;
    if (sscanf(setting, "%ux%u", &width, &height) != 2 || width < 320 || width > 1280 ||
        height < 180 || height > 720)
    {
        LOGFN_WARNING("GTA4_RES={}: expected <width>x<height> up to 1280x720", setting);
        return;
    }
    char* widthText = reinterpret_cast<char*>(g_memory.Translate(0x82000A00));
    char* heightText = reinterpret_cast<char*>(g_memory.Translate(0x820009FC));
    if (strcmp(widthText, "1280") != 0 || strcmp(heightText, "720") != 0)
    {
        LOGN_WARNING("GTA4_RES: the default resolution strings are not where expected");
        return;
    }
    snprintf(widthText, 8, "%u", width);
    snprintf(heightText, 4, "%u", height);
    LOGFN("render resolution set to {}x{}", width, height);
}

// gta4port: the whole game run. main() calls it in the command-line build; the Android app's
// libgta4.so (GTA4_SHARED) starts it on its own thread from runtime/os/android_app.cpp.
int Gta4Main()
{
    os::logger::Init();
    InstallCrashHandler();

    if (g_memory.base == nullptr)
    {
        LOGN_ERROR("cannot reserve the 4 GB guest address space");
        return 1;
    }

    const auto xexPath = os::paths::GetGamePath() / "default.xex";
    LOGFN("game data: {}", os::paths::GetGamePath().string());

    KiSystemStartup();
    StartThreadDumpWatchdog();

    const uint32_t entry = LdrLoadModule(xexPath);
    if (entry == 0)
        return 1;
    ApplyRenderResolution();

    NameCurrentThread("gta4-main");
    try
    {
        GuestThread::Start({ entry, 0, 0 });
    }
    catch (const GuestThreadExit& exit)
    {
        LOGFN("main thread terminated with code {}", exit.exitCode);
    }

    return 0;
}

#ifndef GTA4_SHARED
int main(int argc, char* argv[])
{
    return Gta4Main();
}
#endif
