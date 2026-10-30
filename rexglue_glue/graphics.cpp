// gta4port: the native renderer in the runtime. Starts LibertyRecomp's Gta4NativeGraphicsSystem
// over the runtime's guest memory with what its presenter needs and no ReXGlue application: a UI
// thread of its own (gta4-native-ui) and a Window over the activity's SurfaceView, the same
// arrangement as xenia_glue/graphics.cc. The ported hooks find the renderer through
// GraphicsSystem() (rexglue_glue.h).
//
// GTA4_NATIVE_CAPTURE=<directory>: every 2 s the presented guest frame is saved there as a PNG
// (the PC build runs offscreen, so this is how its frames are seen).
#include "gta4_native.h"
#include "rexglue_glue.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/diagnostics/policy.h>
#include <rex/ui/presenter.h>
#ifdef __ANDROID__
#include <rex/ui/surface_android.h>
#endif
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "graphics/gta4_native/graphics_system.h"

#include <rex/cvar.h>

#include <initializer_list>
#include <string_view>
#include <utility>

namespace rex::graphics::gta4_native {
// graphics_system.cpp (gta4port): decode DXT on the CPU even when the GPU samples BC.
extern bool gta4port_force_dxt_decode;
}  // namespace rex::graphics::gta4_native

#ifdef _WIN32
// pc_window.cpp
void PcWindowThread(std::function<rex::ui::Presenter*()> presenter);
#endif

namespace {

class UiContext final : public rex::ui::WindowedAppContext {
 public:
  // Runs the UI loop on the thread that created the context.
  void Run() {
    while (true) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return pending_; });
        pending_ = false;
      }
      ExecutePendingFunctionsFromUIThread();
    }
  }

 protected:
  void NotifyUILoopOfPendingFunctions() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ = true;
    }
    condition_.notify_one();
  }
  void PlatformQuitFromUIThread() override {}

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool pending_ = false;
};

// A Window over the activity's SurfaceView: fullscreen, no menus, no input (input reaches the
// game through the runtime).
class SurfaceWindow final : public rex::ui::Window {
 public:
  explicit SurfaceWindow(rex::ui::WindowedAppContext& app_context)
      : Window(app_context, "GTA IV", 1280, 720) {}
  ~SurfaceWindow() override { EnterDestructor(); }

  // UI thread.
  void SetNativeWindow(ANativeWindow* window) {
    if (window == native_window_) {
      return;
    }
    native_window_ = nullptr;
    OnSurfaceChanged(false);
    native_window_ = window;
#ifdef __ANDROID__
    if (window) {
      WindowDestructionReceiver destruction_receiver(this);
      OnActualSizeUpdate(uint32_t(ANativeWindow_getWidth(window)),
                         uint32_t(ANativeWindow_getHeight(window)), destruction_receiver);
      OnSurfaceChanged(true);
    }
#endif
  }

 protected:
  bool OpenImpl() override {
    OnDesiredFullscreenUpdate(true);
    return true;
  }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(
      rex::ui::Surface::TypeFlags allowed_types) override {
#ifdef __ANDROID__
    if (native_window_ && (allowed_types & rex::ui::Surface::kTypeFlag_AndroidNativeWindow)) {
      return std::make_unique<rex::ui::AndroidNativeWindowSurface>(native_window_);
    }
#endif
    (void)allowed_types;
    return nullptr;
  }
  void RequestPaintImpl() override {
    app_context().CallInUIThreadDeferred([this]() { OnPaint(); });
  }

 private:
  ANativeWindow* native_window_ = nullptr;
};

struct State {
  rex::memory::Memory* memory = nullptr;
  std::unique_ptr<rex::graphics::gta4_native::Gta4NativeGraphicsSystem> graphics;
  UiContext* ui_context = nullptr;
  SurfaceWindow* window = nullptr;
};

State* g_state = nullptr;
std::atomic<bool> g_active{false};

void StartupStep(const char* step) {
  fprintf(stderr, "gta4native: %s\n", step);
  fflush(stderr);
}

void CaptureThread(std::string directory) {
  for (uint32_t index = 0;; ++index) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    rex::ui::Presenter* presenter = g_state->graphics->presenter();
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image)) {
      continue;
    }
    for (size_t y = 0; y < image.height; ++y) {
      for (size_t x = 0; x < image.width; ++x) {
        image.data[y * image.stride + x * 4 + 3] = 0xFF;  // R8G8B8X8
      }
    }
    char name[64];
    snprintf(name, sizeof(name), "/native_%05u.png", index);
    const std::string path = directory + name;
    stbi_write_png(path.c_str(), int(image.width), int(image.height), 4, image.data.data(),
                   int(image.stride));
    REXLOG_INFO("gta4native: captured {}", path);
  }
}

// Test switches next to the saves (files/ on the phone), read every second while the game runs;
// without a file the configured value stays:
// - frame_limit: a number replaces gta4_frame_limit (0, 20, 30, 40, 60, 120).
// - density: gta4_population_scale (0.6 = 60 % of the cars and pedestrians the game asks for).
// - draw_distance: gta4_draw_distance_scale.
// - fog: gta4_fog_distance, the farthest the camera sees in meters with the fog brought in to
//   cover the cut (700 on Android; 0 = the game's 1500 m; live).
// - draw_distance_auto: gta4_dynamic_draw_distance (1 on, 0 off: the full distance at once);
//   draw_distance_min: its floor, gta4_dynamic_draw_distance_min (0.8 on Android).
// - pacing_mode: 'A' paces on the display's real refresh (vulkan_pacer_refresh_lock), 'B' on
//   1 s / limit as before 2026-10-26.
// - shadow_min: gta4_native_shadow_min_indices (0 = every sun-shadow draw).
// - shadow_size, shadow_range: gta4_shadow_map_base_size (192 on Android, sun map 5x that) and
//   gta4_shadow_distance_scale (1.0 on Android, the console's sun-shadow range); read at startup.
// - shadows: gta4_native_shadow_interval (0 on Android = no sun shadows, 1 = every frame; live).
//   shadow_cascades: gta4_shadow_cascade_count (1 on Android, 0 = the stock 5; at startup).
// - shadow_warp, shadow_hold: gta4_shadow_warp_radius (64 m, 16 = the game's) and
//   gta4_shadow_hold_distance (8 m, 0 = re-centre every frame as the game does); live.
// - fps_guard: gta4_fps_guard (0 = off, as Android starts; 3 = the old guard). Off restores the
//   reflection intervals it had overridden.
// - thermal_ceiling, audio_mix, reuse_lighting, store_none, batch_barriers, batch_detile,
//   early_tests: 1/A on, 0/B off for gta4_thermal_ceiling, gta4_native_audio_mix,
//   gta4_native_reuse_lighting_real, gta4_native_store_op_none,
//   gta4_native_batch_attachment_barriers, gta4_native_batched_detile_barrier,
//   gta4_native_early_fragment_tests (the 2026-10-27 plan's changes, each with its off switch).
// - quality: gta4_resolution_quality, dynamic resolution (nativa, alta, equilibrada, rendimiento,
//   bateria); applies in game.
// The first read is before the first frame, so a switch that only takes effect at startup works
// too.
void PresentationSwitchThread(std::string directory) {
  struct Switch {
    const char* file;
    const char* cvar;
    std::string configured, applied;
  };
  Switch switches[] = {{"frame_limit", "gta4_frame_limit"},
                       {"pacing_mode", "vulkan_pacer_refresh_lock"},
                       {"density", "gta4_population_scale"},
                       {"draw_distance", "gta4_draw_distance_scale"},
                       {"fog", "gta4_fog_distance"},
                       {"draw_distance_auto", "gta4_dynamic_draw_distance"},
                       {"draw_distance_min", "gta4_dynamic_draw_distance_min"},
                       {"shadow_min", "gta4_native_shadow_min_indices"},
                       {"shadow_size", "gta4_shadow_map_base_size"},
                       {"shadow_range", "gta4_shadow_distance_scale"},
                       {"shadows", "gta4_native_shadow_interval"},
                       {"shadow_cascades", "gta4_shadow_cascade_count"},
                       {"shadow_warp", "gta4_shadow_warp_radius"},
                       {"shadow_hold", "gta4_shadow_hold_distance"},
                       {"fps_guard", "gta4_fps_guard"},
                       {"thermal_ceiling", "gta4_thermal_ceiling"},
                       {"audio_mix", "gta4_native_audio_mix"},
                       {"reuse_lighting", "gta4_native_reuse_lighting_real"},
                       {"store_none", "gta4_native_store_op_none"},
                       {"batch_barriers", "gta4_native_batch_attachment_barriers"},
                       {"batch_detile", "gta4_native_batched_detile_barrier"},
                       {"early_tests", "gta4_native_early_fragment_tests"},
                       {"quality", "gta4_resolution_quality"}};
  for (Switch& item : switches) {
    item.configured = item.applied = rex::cvar::GetFlagByName(item.cvar);
  }
  for (;;) {
    for (Switch& item : switches) {
      const bool boolean = item.configured == "true" || item.configured == "false";
      std::string wanted = item.configured;
      if (FILE* file = fopen((directory + item.file).c_str(), "r")) {
        char text[16] = {};
        if (fgets(text, sizeof(text), file)) {
          if (boolean && (text[0] == '0' || text[0] == '1')) {
            wanted = text[0] == '1' ? "true" : "false";
          } else if ((text[0] >= '0' && text[0] <= '9') || (text[0] >= 'a' && text[0] <= 'z')) {
            wanted = text;
            wanted.erase(wanted.find_last_not_of(" \r\n") + 1);
          } else if (text[0] == 'A' || text[0] == 'B') {
            wanted = text[0] == 'A' ? "true" : "false";
          }
        }
        fclose(file);
      }
      if (wanted == item.applied) continue;
      const bool ok = rex::cvar::SetFlagByName(item.cvar, wanted);
      REXLOG_INFO("gta4native: {} {} -> {} ({})", item.file, item.applied, wanted,
                  ok ? "set" : "rejected");
      item.applied = wanted;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace

namespace gta4native {

rex::memory::Memory* GuestMemory() { return g_state ? g_state->memory : nullptr; }

rex::system::IGraphicsSystem* GraphicsSystem() {
  return g_active.load(std::memory_order_acquire) ? g_state->graphics.get() : nullptr;
}

rex::ui::Window* DisplayWindow() { return g_state ? g_state->window : nullptr; }

bool IsActive() { return g_active.load(std::memory_order_relaxed); }

bool Initialize(uint8_t* guest_base, uint8_t* physical_base) {
  if (g_state) {
    return g_active;
  }
  // The renderer's log (REXLOG_*): GTA4_NATIVE_LOG=<file>; by default gta4native.log on the PC,
  // logcat only on Android.
  // ReXGlue logs only when its diagnostics policy enables the logging category;
  // GTA4_NATIVE_DIAG=<comma list> (rex::diagnostics categories, e.g. logging,native-trace) or all.
  {
    const char* diag = getenv("GTA4_NATIVE_DIAG");
    rex::diagnostics::Configure(true, diag && *diag ? diag : "logging");
  }
  rex::InitLoggingEarly();
  const char* log_file = getenv("GTA4_NATIVE_LOG");
#ifndef __ANDROID__
  if (!log_file || !*log_file) {
    log_file = "gta4native.log";
  }
#endif
  rex::InitLogging(log_file && *log_file ? log_file : nullptr, spdlog::level::info);

  // GTA4_NATIVE_ANDROID_PATH=1 (PC): the settings Android builds use (REX_PLATFORM_ANDROID
  // defaults) plus CPU DXT decoding, to reproduce phone-only code paths on the PC.
  // GTA4_NATIVE_CVARS="name=value,...": any ReXGlue / renderer option, applied after it.
  if (const char* android_path = getenv("GTA4_NATIVE_ANDROID_PATH");
      android_path && android_path[0] == '1') {
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"gta4_shadow_map_base_size", "256"},
             {"gta4_reflection_resolution", "original"},
             {"gta4_native_anti_aliasing", "fxaa"},
             {"gta4_draw_distance_scale", "1.0"},
             {"gta4_motion_blur", "false"},
             {"gta4_native_early_fragment_tests", "false"}}) {
      rex::cvar::SetFlagByName(name, value);
    }
    rex::graphics::gta4_native::gta4port_force_dxt_decode = true;
  }
  // Vector font replacement needs LibertyRecomp's font atlases: in the resources folder (on
  // Android MainActivity copies them there from the APK and sets REX_RESOURCES_DIR) or at
  // GTA4_NATIVE_FONT_ASSET_ROOT (a build-time path that only exists on the PC); font_atlas.cpp
  // tries them in that order. Without them every font texture failed its cache identity check and
  // was decoded again on every draw (~900 DXT5 512x512 decodes/s on the phone), so the game's own
  // fonts are used instead.
  {
    std::error_code error;
    const auto resources = rex::filesystem::GetResourcesFolder() / "font_atlases" / "font1.png";
    if (std::filesystem::is_regular_file(resources, error)) {
      fprintf(stderr, "gta4native: font atlases in %s\n", resources.parent_path().string().c_str());
    } else if (!std::filesystem::exists(GTA4_NATIVE_FONT_ASSET_ROOT, error)) {
      rex::cvar::SetFlagByName("gta4_native_vector_fonts", "false");
      fprintf(stderr, "gta4native: no font atlases at %s, vector fonts off\n",
              GTA4_NATIVE_FONT_ASSET_ROOT);
    }
  }
  if (const char* list = getenv("GTA4_NATIVE_CVARS"); list && *list) {
    std::string_view rest(list);
    while (!rest.empty()) {
      const size_t comma = rest.find(',');
      const std::string_view item = rest.substr(0, comma);
      rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
      const size_t equals = item.find('=');
      if (equals == std::string_view::npos) {
        continue;
      }
      const bool ok = rex::cvar::SetFlagByName(item.substr(0, equals), item.substr(equals + 1));
      fprintf(stderr, "gta4native: option %.*s %s\n", int(item.size()), item.data(),
              ok ? "set" : "UNKNOWN");
    }
  }

  auto* state = new State;
  state->memory = CreateMemory(guest_base, physical_base);

  // The UI thread creates the context (which binds it to that thread) and the window, then runs
  // the loop for the rest of the process.
  std::mutex ready_mutex;
  std::condition_variable ready_condition;
  bool ready = false;
  StartupStep("starting the UI thread");
  std::thread([&]() {
    auto* context = new UiContext;
    auto* window = new SurfaceWindow(*context);
    window->Open();
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      state->ui_context = context;
      state->window = window;
      ready = true;
    }
    ready_condition.notify_one();
    context->Run();
  }).detach();
  {
    std::unique_lock<std::mutex> lock(ready_mutex);
    ready_condition.wait(lock, [&] { return ready; });
  }
  g_state = state;

  StartupStep("creating Vulkan and the native renderer");
  state->graphics = std::make_unique<rex::graphics::gta4_native::Gta4NativeGraphicsSystem>();
  if (XFAILED(state->graphics->SetupPresentation(state->ui_context))) {
    StartupStep("presentation setup failed");
    return false;
  }
  // Persistent pipeline cache next to the saves (on the phone: the app's files directory), so
  // pipelines are not compiled again on every launch.
  {
    const char* save_dir = getenv("GTA4_SAVE_DIR");
    const std::filesystem::path cache_root =
        save_dir && *save_dir ? std::filesystem::path(save_dir).parent_path() / "native-cache"
                              : std::filesystem::path("native-cache");
    std::error_code error;
    std::filesystem::create_directories(cache_root, error);
    state->graphics->InitializeShaderStorage(cache_root, rex::graphics::gta4_native::kTitleId,
                                             true);
  }
  if (XFAILED(state->graphics->SetupGuestGpuWithMemory(state->memory))) {
    StartupStep("guest GPU setup failed");
    return false;
  }
  state->ui_context->CallInUIThreadSynchronous(
      [state]() { state->window->SetPresenter(state->graphics->presenter()); });

  if (const char* capture = getenv("GTA4_NATIVE_CAPTURE"); capture && *capture) {
    std::thread(CaptureThread, std::string(capture)).detach();
  }
#ifdef _WIN32
  if (const char* window = getenv("GTA4_PC_WINDOW"); window && window[0] == '1') {
    std::thread(PcWindowThread, [] { return g_state->graphics->presenter(); }).detach();
  }
#endif
  StartPopulationScaling(guest_base);
  StartShadowRangeWatch(guest_base);
  if (const char* save_dir = getenv("GTA4_SAVE_DIR"); save_dir && *save_dir) {
    std::thread(PresentationSwitchThread, std::string(save_dir) + "/../").detach();
  }
  g_active.store(true, std::memory_order_release);
  StartupStep("ready");
  return true;
}

void SetSurface(ANativeWindow* window) {
  if (!g_state) {
    return;
  }
  g_state->ui_context->CallInUIThreadSynchronous(
      [window]() { g_state->window->SetNativeWindow(window); });
}

}  // namespace gta4native
