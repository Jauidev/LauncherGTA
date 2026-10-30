#pragma once

#include <cstdint>

namespace rex::graphics::gta4_native {

// Guest addresses this tree compares against or reads. tools/gen_iv_sources.py translates runtime/
// and rexglue_glue/ for GTA IV but not third_party/, so the two values live here.
struct GuestAddresses {
  uint32_t local_light_loop;      // sub_822C0F08
  uint32_t composite_postfx;      // sub_822E3E98
  uint32_t new_draw_list_vtable;  // CNewDrawListDC
  uint32_t tv_script_name;        // "script_rt"
  uint32_t draw_indexed_label;    // sub_82A3DF38, only a label in traces
  uint32_t game_clock;            // read by sub_82347008
};

#ifdef GTA4_IV
inline constexpr GuestAddresses kGuestAddresses{0x822EA370, 0x822E4928, 0x82000B38,
                                                0x82001638, 0x829CCEF8, 0x82CFA278};
#else
inline constexpr GuestAddresses kGuestAddresses{0x822C0F08, 0x822E3E98, 0x82001038,
                                                0x82001950, 0x82A3DF38, 0x82DF37EC};
#endif

}  // namespace rex::graphics::gta4_native
