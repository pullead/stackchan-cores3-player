#pragma once

#include <lvgl.h>

// Visual language carried over from the esp32-hifi player.
//
// The palette and metrics are reproduced so the player looks and behaves the
// same as on the original 320x170 board.  The code is a fresh LVGL 9
// implementation -- the upstream UI targets LVGL 8.3, whose display/input
// driver types no longer exist here -- but every colour, size and offset below
// matches the original, including the reasons recorded in its comments.
namespace hifi_theme {

// Palette (upstream src/ui/hifi_ui.cpp).
inline lv_color_t bg() { return lv_color_hex(0x0A0B12); }
inline lv_color_t status_bar() { return lv_color_hex(0x05060A); }
inline lv_color_t panel() { return lv_color_hex(0x161A2B); }
inline lv_color_t panel_deep() { return lv_color_hex(0x0E1019); }
inline lv_color_t ink() { return lv_color_hex(0xF2F3F7); }
inline lv_color_t ink_dim() { return lv_color_hex(0xB0B6C8); }
inline lv_color_t ink_faint() { return lv_color_hex(0x565C70); }
inline lv_color_t accent() { return lv_color_hex(0xA855F7); }
inline lv_color_t accent_bright() { return lv_color_hex(0xC77DFF); }
inline lv_color_t accent_deep() { return lv_color_hex(0x6D28D9); }
inline lv_color_t magenta() { return lv_color_hex(0xD946EF); }
inline lv_color_t ok() { return lv_color_hex(0x34D399); }
inline lv_color_t mute() { return lv_color_hex(0xE4574B); }

// Metrics.  The original panel is 320x170; CoreS3 gives 70px more height, and
// per the porting decision that extra height goes to the list and nothing else
// moves.
inline constexpr int32_t kScreenWidth = 320;
inline constexpr int32_t kScreenHeight = 240;
inline constexpr int32_t kUpstreamHeight = 170;
inline constexpr int32_t kExtraHeight = kScreenHeight - kUpstreamHeight;

// Track list: rows are 28 high with a 4px gap, so the pitch is 32.  Upstream
// measured this as the single biggest factor in how many rows fit -- dropping
// the pitch from 44 to 32 took the visible count from 2.9 to 4.25 rows.
inline constexpr int32_t kRowHeight = 28;
inline constexpr int32_t kRowPitch = 32;
inline constexpr int32_t kRowWidth = 278;
// Upstream measured round corners as the dominant scroll cost: at radius 10 a
// single LVGL handler blocked for 42-47ms, at radius 0 for 21-30ms.  4 keeps
// the look while cutting the corner area to about 16% of radius 10.
inline constexpr int32_t kRowRadius = 4;
inline constexpr int32_t kGroupRowRadius = 10;

// Tab strip: five 36x20 chips on a 40px pitch starting at x=8, y=4.
inline constexpr int32_t kTabCount = 5;
inline constexpr int32_t kTabX = 8;
inline constexpr int32_t kTabY = 4;
inline constexpr int32_t kTabWidth = 36;
inline constexpr int32_t kTabHeight = 20;
inline constexpr int32_t kTabPitch = 40;
inline constexpr int32_t kTabRadius = 10;

// List viewport: upstream is 286x136 at (8, 28), narrowed from 304 to leave
// room for the draggable scroll slider at x=300.
inline constexpr int32_t kListX = 8;
inline constexpr int32_t kListY = 28;
inline constexpr int32_t kListWidth = 286;
inline constexpr int32_t kListHeight = 136 + kExtraHeight;
inline constexpr int32_t kScrollSliderX = 300;
inline constexpr int32_t kScrollSliderWidth = 12;

}  // namespace hifi_theme
