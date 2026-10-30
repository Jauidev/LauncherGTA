// gta4port: the graphics settings and the anti-aliasing / HDR controllers the native renderer and
// its hooks read, taken verbatim from LibertyRecomp's gta4_app.cpp (GPL-3) lines 108-565; the
// rest of that file is their application (window, installer, multiplayer) and is not used.
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/logging.h>
#include <rex/graphics/gta4_native/anti_aliasing_policy.h>
#include <rex/graphics/gta4_native/hdr_policy.h>
#include <rex/graphics/gta4_native/supersampling_policy.h>
#include <rex/graphics/gta4_native/title_commands.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/window.h>

#include "rexglue_glue.h"



REXCVAR_DEFINE_STRING(gta4_aspect_ratio, "auto", "GTA IV/Graphics/Display",
                      "Render aspect: auto follows the display; fixed ratios fit without stretching")
    .allowed({"auto", "original", "16:9", "16:10", "3:2", "4:3", "5:4", "21:9", "43:18", "32:9", "32:10"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_present_mode, "vsync", "GTA IV/Graphics/Display",
                      "Vertical sync: vsync is On; immediate is Off; auto/mailbox are legacy modes")
    .allowed({"auto", "vsync", "fifo", "mailbox", "immediate"});
// gta4port, Android: 30 FPS (2 refreshes of the 60 Hz panel), held by the dynamic draw distance
// below. Below the cap frames reach the panel after 2, 3 or 4 refreshes and driving judders; 20
// (files/frame_limit) keeps every frame at 3 refreshes when 30 is out of reach.
REXCVAR_DEFINE_UINT32(gta4_frame_limit, REX_PLATFORM_ANDROID ? 30 : 60, "GTA IV/Graphics/Display",
                      "Maximum game and host presentation rate: 0 is unlocked; caps are 20, 30, 40, "
                      "60, and 120 FPS")
    .allowed({"0", "20", "30", "40", "60", "120"});
REXCVAR_DEFINE_STRING(gta4_native_hdr_mode, "off", "GTA IV/Graphics/HDR",
                      "HDR output: off, scRGB, or Auto HDR")
    .allowed({"off", "scrgb", "auto_hdr"});
REXCVAR_DEFINE_BOOL(gta4_native_hdr_mode_unified, false,
                    "GTA IV/Graphics/HDR/Compatibility",
                    "Canonical HDR mode has replaced the legacy Vulkan HDR toggle");
REXCVAR_DEFINE_DOUBLE(gta4_native_hdr_paper_white_nits, 203.0,
                      "GTA IV/Graphics/HDR",
                      "SDR reference white brightness used by scRGB and Auto HDR")
    .range(80.0, 500.0);
REXCVAR_DEFINE_DOUBLE(gta4_native_hdr_peak_nits, 400.0, "GTA IV/Graphics/HDR",
                      "Auto HDR highlight target brightness")
    .range(80.0, 2000.0);
REXCVAR_DEFINE_DOUBLE(gta4_native_auto_hdr_shoulder_start, 0.0,
                      "GTA IV/Graphics/HDR/Advanced",
                      "Normalized luminance where Auto HDR highlight expansion begins")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(gta4_native_auto_hdr_shoulder_power, 2.5,
                      "GTA IV/Graphics/HDR/Advanced",
                      "Auto HDR highlight shoulder exponent")
    .range(1.0, 10.0);

REXCVAR_DEFINE_UINT32(gta4_shadow_map_base_size, REX_PLATFORM_ANDROID ? 256 : 512,
                      "GTA IV/Graphics/Shadows",
                      "Base shadow-map size (512 creates a 4096x4096 point-shadow cache; "
                      "192 makes the sun's map 960x960)")
    .range(128, 1024)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(gta4_shadow_distance_guard_factor, 1.0, "GTA IV/Graphics/Shadows",
                      "Multiplier on the shadow distance owned by the 30 FPS guard")
    .range(0.1, 1.0);
REXCVAR_DEFINE_DOUBLE(gta4_shadow_distance_scale, 2.0, "GTA IV/Graphics/Shadows",
                      "Multiplier applied to GTA IV's directional shadow range")
    .range(0.1, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_reflection_resolution, REX_PLATFORM_ANDROID ? "original" : "1080p",
                      "GTA IV/Graphics/Reflections",
                      "Reflection resolution preset: original, 1080p, or full")
    .allowed({"original", "1080p", "full"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_reflection_resolution_cap, "1440p", "GTA IV/Graphics/Reflections",
                      "Maximum Full reflection resolution: 1080p, 1440p, or display")
    .allowed({"1080p", "1440p", "display"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_mirror_reflection_resolution, "inherit",
                      "GTA IV/Graphics/Reflections/Advanced",
                      "Mirror reflection override: inherit, original, 1080p, or full")
    .allowed({"inherit", "original", "1080p", "full"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_water_reflection_resolution, "inherit",
                      "GTA IV/Graphics/Reflections/Advanced",
                      "Water reflection override: inherit, original, 1080p, or full")
    .allowed({"inherit", "original", "1080p", "full"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_environment_reflection_resolution, "inherit",
                      "GTA IV/Graphics/Reflections/Advanced",
                      "Vehicle/world environment reflection override: inherit, original, 1080p, "
                      "or full")
    .allowed({"inherit", "original", "1080p", "full"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_reflection_aa, "original", "GTA IV/Graphics/Reflections",
                      "Native reflection capture anti-aliasing: original, off, 2x, or 4x")
    .allowed({"original", "off", "2x", "4x"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_reflection_capture_distance, "original",
                      "GTA IV/Graphics/Reflections/Advanced",
                      "Exterior environment capture distance: near (20), original (40), extended (60), or "
                      "far (80)")
    .allowed({"near", "original", "extended", "far"});
REXCVAR_DEFINE_STRING(gta4_native_anti_aliasing, REX_PLATFORM_ANDROID ? "fxaa" : "smaa",
                      "GTA IV/Graphics/Anti-Aliasing",
                      "Anti-aliasing: off, fxaa, smaa, deferred MSAA, or even-factor SSAA")
    // `spatial` remains loadable as a deprecated compatibility alias. It is
    // never exposed by the frontend or accepted by the controller setter.
    .allowed({"off", "fxaa", "smaa", "taa", "metalfx_taa", "msaa2x", "msaa4x", "msaa4x_smaa", "ssaa2x", "ssaa4x",
              "ssaa6x", "ssaa8x", "ssaa10x", "ssaa12x", "ssaa14x", "ssaa16x",
              "spatial"});
REXCVAR_DEFINE_BOOL(gta4_metalfx_frame_generation, false, "GTA IV/Graphics/Display",
                    "MetalFX 2x frame interpolation; requires temporal AA and synchronized display")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_native_frame_generation, "off", "GTA IV/Graphics/Display",
                      "Vulkan frame interpolation: off, fsr3, or dlss; requires FSR 3.1 or DLSS temporal reconstruction")
    .allowed({"off", "fsr3", "dlss"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gta4_native_anti_aliasing_unified, false,
                    "GTA IV/Graphics/Anti-Aliasing/Compatibility",
                    "Canonical unified anti-aliasing selection has replaced legacy split values");
REXCVAR_DEFINE_STRING(gta4_native_smaa_quality, "high", "GTA IV/Graphics/Anti-Aliasing",
                      "SMAA 1x preset: low, medium, high, or ultra")
    .allowed({"low", "medium", "high", "ultra"});
REXCVAR_DEFINE_STRING(gta4_native_upscaler, "native", "GTA IV/Graphics/Upscaling",
                      "Output upscaler: native, fsr1, MetalFX, FSR 3.1, or DLSS; temporal modes require backend support")
    .allowed({"native", "fsr1", "metalfx", "fsr3", "dlss"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_temporal_upscaler_quality, "quality", "GTA IV/Graphics/Upscaling",
                      "FSR 3.1/DLSS preset; dimensions come from the active provider")
    .allowed({"native", "quality", "balanced", "performance", "ultra_performance"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gta4_fsr1_quality, "quality", "GTA IV/Graphics/Upscaling",
                      "FSR 1 preset: ultra_quality, quality, balanced, or performance")
    .allowed({"ultra_quality", "quality", "balanced", "performance"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(gta4_fsr1_sharpness_reduction, 0.2, "GTA IV/Graphics/Upscaling",
                      "FSR 1 RCAS sharpness reduction in stops")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Keep the authored resident LOD hierarchy by default. Forcing LOD0 at every
// distance bypasses transitions as soon as a new drawable becomes resident.
REXCVAR_DEFINE_BOOL(gta4_force_highest_lod, false, "GTA IV/Graphics/LOD",
                    "Prefer the highest resident model LOD regardless of distance");
// gta4port: the controller (UpdateDynamicDrawDistance) trades distance for the 30 FPS target: the
// recorder and the game's render thread both work per drawn object. Off on the PC. On Android it
// was turned off on 2026-10-27 (it went as low as 0.6 on any late frame) and came back the same day
// softer: full distance unless the recorder's own work is what misses the target, then down to 0.8
// at most. files/draw_distance_auto = 0 turns it off in game, files/draw_distance_min sets the floor.
REXCVAR_DEFINE_BOOL(gta4_dynamic_draw_distance, REX_PLATFORM_ANDROID ? true : false, "GTA IV/Graphics/LOD",
                    "Lower the draw distance while frames miss the frame-rate target, restore it with headroom");
REXCVAR_DEFINE_DOUBLE(gta4_dynamic_draw_distance_min, REX_PLATFORM_ANDROID ? 0.8 : 0.7, "GTA IV/Graphics/LOD",
                      "Lowest fraction of the configured draw distance the dynamic controller may use")
    .range(0.25, 1.0);
// Written by the renderer's controller; multiplies gta4_draw_distance_scale.
REXCVAR_DEFINE_DOUBLE(gta4_draw_distance_dynamic_factor, 1.0, "GTA IV/Graphics/LOD",
                      "Current dynamic draw-distance factor (managed automatically)")
    .range(0.25, 1.0);
REXCVAR_DEFINE_DOUBLE(gta4_draw_distance_scale, REX_PLATFORM_ANDROID ? 1.0 : 3.0,
                      "GTA IV/Graphics/LOD",
                      "Multiplier applied through GTA IV's built-in world-distance input")
    .range(0.25, 4.0);
REXCVAR_DEFINE_UINT32(gta4_drawable_reference_limit, 20000, "GTA IV/Graphics/LOD",
                      "Drawable-reference capacity used by extended draw distances")
    .range(13000, 40000)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

std::mutex g_anti_aliasing_controller_mutex;
std::atomic<bool> g_anti_aliasing_controller_initialized{false};
std::atomic<rex::graphics::gta4_native::AntiAliasingMode>
    g_configured_anti_aliasing_mode{rex::graphics::gta4_native::AntiAliasingMode::kSmaa};
std::atomic<rex::graphics::gta4_native::AntiAliasingMode>
    g_active_anti_aliasing_mode{rex::graphics::gta4_native::AntiAliasingMode::kSmaa};
std::mutex g_hdr_controller_mutex;
std::atomic<bool> g_hdr_controller_initialized{false};
std::atomic<rex::graphics::gta4_native::HdrMode> g_configured_hdr_mode{
    rex::graphics::gta4_native::HdrMode::kOff};

const char* AntiAliasingCompatibilityName(
    rex::graphics::gta4_native::AntiAliasingCompatibility compatibility) {
  using Compatibility = rex::graphics::gta4_native::AntiAliasingCompatibility;
  switch (compatibility) {
    case Compatibility::kCanonical:
      return "canonical";
    case Compatibility::kSpatialAlias:
      return "spatial-alias";
    case Compatibility::kLegacySceneMsaa:
      return "legacy-scene-msaa";
    case Compatibility::kLegacySpatialToggle:
      return "legacy-spatial-toggle";
    case Compatibility::kFallbackOff:
      return "fallback-off";
  }
  return "fallback-off";
}

void InitializeFrontendAntiAliasingControllerLocked() {
  using namespace rex::graphics::gta4_native;
  if (g_anti_aliasing_controller_initialized.load(std::memory_order_acquire)) {
    return;
  }
  const std::string configured = REXCVAR_GET(gta4_native_anti_aliasing);
  const std::string legacy_scene_msaa = rex::cvar::GetFlagByName("gta4_native_msaa");
  const auto resolved = ResolveAntiAliasingConfiguration(
      configured, legacy_scene_msaa,
      rex::cvar::Query<bool>("gta4_native_spatial_aa"),
      REXCVAR_GET(gta4_native_anti_aliasing_unified));
  g_configured_anti_aliasing_mode.store(resolved.mode, std::memory_order_release);
  g_active_anti_aliasing_mode.store(resolved.mode, std::memory_order_release);
  g_anti_aliasing_controller_initialized.store(true, std::memory_order_release);
  const auto route = GetAntiAliasingRoute(resolved.mode);
  REXLOG_INFO(
      "GTA4AAPolicy owner=frontend event=initialize configured-raw={} legacy-msaa={} "
      "legacy-spatial={} unified={} configured={} active={} compatibility={} "
      "ignored-legacy-msaa={} route-fxaa={} route-smaa={} scene-samples={} ssaa-factor={}",
      configured, legacy_scene_msaa, rex::cvar::Query<bool>("gta4_native_spatial_aa"),
      REXCVAR_GET(gta4_native_anti_aliasing_unified), AntiAliasingModeName(resolved.mode),
      AntiAliasingModeName(resolved.mode), AntiAliasingCompatibilityName(resolved.compatibility),
      resolved.ignored_legacy_scene_msaa, route.presentation_fxaa, route.presentation_smaa,
      route.scene_sample_count, route.supersampling_pixel_factor);
}

const char* HdrCompatibilityName(
    rex::graphics::gta4_native::HdrCompatibility compatibility) {
  using Compatibility = rex::graphics::gta4_native::HdrCompatibility;
  switch (compatibility) {
    case Compatibility::kCanonical:
      return "canonical";
    case Compatibility::kLegacyVulkanHdr:
      return "legacy-vulkan-hdr";
    case Compatibility::kFallbackOff:
      return "fallback-off";
  }
  return "fallback-off";
}

void InitializeFrontendHdrControllerLocked() {
  using namespace rex::graphics::gta4_native;
  if (g_hdr_controller_initialized.load(std::memory_order_acquire)) {
    return;
  }
  const std::string configured = REXCVAR_GET(gta4_native_hdr_mode);
  const bool legacy_vulkan_hdr = rex::cvar::Query<bool>("vulkan_hdr");
  const auto resolved = ResolveHdrConfiguration(
      configured, legacy_vulkan_hdr, REXCVAR_GET(gta4_native_hdr_mode_unified));
  const std::string_view resolved_name = HdrModeName(resolved.mode);
  const bool transport_enabled = HdrModeNeedsExtendedOutput(resolved.mode);
  const bool canonical_written =
      rex::cvar::SetFlagByName("gta4_native_hdr_mode", resolved_name);
  const bool marker_written =
      rex::cvar::SetFlagByName("gta4_native_hdr_mode_unified", "true");
  const bool transport_written = rex::cvar::SetFlagByName(
      "vulkan_hdr", transport_enabled ? "true" : "false");
  if (!canonical_written || !marker_written || !transport_written) {
    REXLOG_ERROR(
        "GTA4HDRPolicy event=initialize-failed configured-raw={} legacy-vulkan-hdr={} "
        "resolved={} canonical-written={} marker-written={} transport-written={}",
        configured, legacy_vulkan_hdr, resolved_name, canonical_written, marker_written,
        transport_written);
  }
  g_configured_hdr_mode.store(resolved.mode, std::memory_order_release);
  g_hdr_controller_initialized.store(true, std::memory_order_release);
  REXLOG_INFO(
      "GTA4HDRPolicy event=initialize configured-raw={} legacy-vulkan-hdr={} unified={} "
      "configured={} compatibility={} transport={}",
      configured, legacy_vulkan_hdr, REXCVAR_GET(gta4_native_hdr_mode_unified), resolved_name,
      HdrCompatibilityName(resolved.compatibility), transport_enabled);
}

bool ValidateSupersamplingSelection(
    rex::graphics::gta4_native::AntiAliasingMode mode) {
  using namespace rex::graphics::gta4_native;
  const uint32_t pixel_factor = GetAntiAliasingRoute(mode).supersampling_pixel_factor;
  if (pixel_factor == 1u) {
    return true;
  }

  // gta4port: rexglue_glue's renderer and window instead of rex::Runtime's.
  auto* graphics = ::gta4native::GraphicsSystem();
  auto* window = ::gta4native::DisplayWindow();
  if (!graphics || graphics->GetTitleCommandAbi(kTitleId) != kTitleCommandAbi || !window) {
    REXLOG_ERROR(
        "GTA4SSAA event=reject-selection factor={} reason=device-capabilities-unavailable",
        pixel_factor);
    return false;
  }

  uint32_t output_width = window->GetActualPhysicalWidth();
  uint32_t output_height = window->GetActualPhysicalHeight();
  int32_t preset_width = 0;
  int32_t preset_height = 0;
  if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                     preset_height) &&
      preset_width > 0 && preset_height > 0) {
    output_width = uint32_t(preset_width);
    output_height = uint32_t(preset_height);
  }

  QueryDeviceCapabilitiesCommand command{};
  DeviceCapabilitiesResult capabilities{};
  if (!output_width || !output_height ||
      !graphics->ExecuteTitleCommand(kTitleId, kTitleCommandAbi, &command, sizeof(command),
                                     &capabilities, sizeof(capabilities))) {
    REXLOG_ERROR(
        "GTA4SSAA event=reject-selection factor={} output={}x{} "
        "reason=device-query-failed",
        pixel_factor, output_width, output_height);
    return false;
  }

  const auto physical = CalculateSupersampledExtent(
      output_width, output_height, pixel_factor, capabilities.max_image_dimension_2d);
  if (!physical) {
    REXLOG_ERROR(
        "GTA4SSAA event=reject-selection factor={} output={}x{} max-dimension={} "
        "reason=physical-extent-unsupported",
        pixel_factor, output_width, output_height, capabilities.max_image_dimension_2d);
    return false;
  }
  REXLOG_INFO(
      "GTA4SSAA event=validate-selection factor={} logical={}x{} physical={}x{} "
      "max-dimension={}",
      pixel_factor, output_width, output_height, physical->width, physical->height,
      capabilities.max_image_dimension_2d);
  return true;
}

}  // namespace

namespace rex::graphics::gta4_native {

void InitializeHdrController() {
  std::lock_guard lock(g_hdr_controller_mutex);
  InitializeFrontendHdrControllerLocked();
}

HdrMode GetConfiguredHdrMode() {
  InitializeHdrController();
  return g_configured_hdr_mode.load(std::memory_order_acquire);
}

std::string_view GetConfiguredHdrModeName() {
  return HdrModeName(GetConfiguredHdrMode());
}

bool SetConfiguredHdrMode(std::string_view value) {
  const auto requested = ParseHdrMode(value);
  if (!requested) {
    REXLOG_ERROR("GTA4HDRPolicy event=reject requested={} reason=invalid-mode", value);
    return false;
  }
  std::lock_guard lock(g_hdr_controller_mutex);
  InitializeFrontendHdrControllerLocked();
  const HdrMode previous =
      g_configured_hdr_mode.load(std::memory_order_acquire);
  const bool transport_enabled = HdrModeNeedsExtendedOutput(*requested);
  if (!rex::cvar::SetFlagByName("gta4_native_hdr_mode_unified", "true") ||
      !rex::cvar::SetFlagByName("gta4_native_hdr_mode", HdrModeName(*requested)) ||
      !rex::cvar::SetFlagByName("vulkan_hdr",
                                transport_enabled ? "true" : "false")) {
    REXLOG_ERROR("GTA4HDRPolicy event=reject requested={} reason=cvar-write", value);
    return false;
  }
  g_configured_hdr_mode.store(*requested, std::memory_order_release);
  REXLOG_INFO(
      "GTA4HDRPolicy event=set previous={} configured={} transport={} apply=live",
      HdrModeName(previous), HdrModeName(*requested), transport_enabled);
  return true;
}

void InitializeAntiAliasingController() {
  std::lock_guard lock(g_anti_aliasing_controller_mutex);
  InitializeFrontendAntiAliasingControllerLocked();
}

AntiAliasingMode GetConfiguredAntiAliasingMode() {
  InitializeAntiAliasingController();
  return g_configured_anti_aliasing_mode.load(std::memory_order_acquire);
}

AntiAliasingMode GetActiveAntiAliasingMode() {
  InitializeAntiAliasingController();
  return g_active_anti_aliasing_mode.load(std::memory_order_acquire);
}

std::string_view GetConfiguredAntiAliasingModeName() {
  return AntiAliasingModeName(GetConfiguredAntiAliasingMode());
}

std::string_view GetActiveAntiAliasingModeName() {
  return AntiAliasingModeName(GetActiveAntiAliasingMode());
}

AntiAliasingApplyResult SetConfiguredAntiAliasingMode(std::string_view value) {
  const auto requested = ParseAntiAliasingMode(value);
  if (!requested) {
    REXLOG_ERROR("GTA4AAPolicy owner=frontend event=reject requested={}", value);
    return AntiAliasingApplyResult::kRejected;
  }

  if(UsesTemporalAntiAliasing(*requested)&&rex::cvar::GetFlagByName("gpu_plugin")!="gta4-metal"){
    REXLOG_WARN("GTA4AAPolicy: {} requires the Metal renderer",value);
    return AntiAliasingApplyResult::kRejected;
  }
  if(rex::cvar::GetFlagByName("gta4_native_upscaler")=="metalfx"&&*requested!=AntiAliasingMode::kMetalFxTaa){
    REXLOG_WARN("GTA4AAPolicy: MetalFX upscaling includes temporal AA; select Native upscaling before changing AA");
    return AntiAliasingApplyResult::kRejected;
  }
  const auto upscaler = rex::cvar::GetFlagByName("gta4_native_upscaler");
  if ((upscaler == "fsr3" || upscaler == "dlss") &&
      (UsesSceneMsaa(*requested) || UsesSceneSupersampling(*requested))) {
    REXLOG_WARN("GTA4AAPolicy: {} requires a single-sample scene; select Native upscaling before {}",
                upscaler, value);
    return AntiAliasingApplyResult::kRejected;
  }
  std::lock_guard lock(g_anti_aliasing_controller_mutex);
  InitializeFrontendAntiAliasingControllerLocked();
  const AntiAliasingMode active =
      g_active_anti_aliasing_mode.load(std::memory_order_acquire);
  const AntiAliasingMode previous_configured =
      g_configured_anti_aliasing_mode.load(std::memory_order_acquire);
  if (UsesSceneSupersampling(*requested) &&
      rex::cvar::GetFlagByName("gta4_native_upscaler") != "native") {
    REXLOG_ERROR(
        "GTA4AAPolicy owner=frontend event=reject requested={} reason=upscaler-conflict "
        "upscaler={}",
        value, rex::cvar::GetFlagByName("gta4_native_upscaler"));
    return AntiAliasingApplyResult::kRejected;
  }
  if (!ValidateSupersamplingSelection(*requested)) {
    return AntiAliasingApplyResult::kRejected;
  }
  const bool apply_live = CanApplyAntiAliasingLive(active, *requested);

  if (!rex::cvar::SetFlagByName("gta4_native_anti_aliasing_unified", "true")) {
    REXLOG_ERROR("GTA4AAPolicy owner=frontend event=reject requested={} reason=unified-marker",
                 value);
    return AntiAliasingApplyResult::kRejected;
  }
  if (!apply_live) {
    std::string_view legacy_scene_msaa = "original";
    if (*requested == AntiAliasingMode::kMsaa2x) {
      legacy_scene_msaa = "2x";
    } else if (GetAntiAliasingRoute(*requested).scene_sample_count == 4u) {
      legacy_scene_msaa = "4x";
    }
    // SetFlagByName intentionally records a pending restart even if the value
    // already matches. The active renderer route remains latched separately.
    if (!rex::cvar::SetFlagByName("gta4_native_msaa", legacy_scene_msaa)) {
      REXLOG_ERROR(
          "GTA4AAPolicy owner=frontend event=reject requested={} reason=restart-tracking",
          value);
      return AntiAliasingApplyResult::kRejected;
    }
  }
  if (!rex::cvar::SetFlagByName("gta4_native_anti_aliasing",
                                AntiAliasingModeName(*requested))) {
    REXLOG_ERROR("GTA4AAPolicy owner=frontend event=reject requested={} reason=canonical-cvar",
                 value);
    return AntiAliasingApplyResult::kRejected;
  }

  g_configured_anti_aliasing_mode.store(*requested, std::memory_order_release);
  if (apply_live) {
    g_active_anti_aliasing_mode.store(*requested, std::memory_order_release);
  }
  const AntiAliasingMode resulting_active =
      g_active_anti_aliasing_mode.load(std::memory_order_acquire);
  const auto route = GetAntiAliasingRoute(resulting_active);
  REXLOG_INFO(
      "GTA4AAPolicy owner=frontend event=set previous-configured={} requested={} active-before={} "
      "active-after={} apply={} route-fxaa={} route-smaa={} scene-samples={} ssaa-factor={}",
      AntiAliasingModeName(previous_configured), AntiAliasingModeName(*requested),
      AntiAliasingModeName(active), AntiAliasingModeName(resulting_active),
      apply_live ? "live" : "restart", route.presentation_fxaa, route.presentation_smaa,
      route.scene_sample_count, route.supersampling_pixel_factor);
  return apply_live ? AntiAliasingApplyResult::kAppliedLive
                    : AntiAliasingApplyResult::kRestartRequired;
}

}  // namespace rex::graphics::gta4_native
