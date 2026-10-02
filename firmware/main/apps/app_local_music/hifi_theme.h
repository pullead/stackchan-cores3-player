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

// --- Status bar ----------------------------------------------------------
// 20px strip: back chevron, clock, WiFi, sample rate, a bordered DAC tag,
// codec name, and volume packed from the right edge.
inline constexpr int32_t kStatusBarHeight = 20;
inline constexpr int32_t kStatusBackWidth = 26;
inline constexpr int32_t kStatusTimeX = 28;
inline constexpr int32_t kStatusWifiX = 68;
inline constexpr int32_t kStatusRateX = 96;
inline constexpr int32_t kStatusDacX = 158;
inline constexpr int32_t kStatusDacWidth = 32;
inline constexpr int32_t kStatusDacHeight = 14;
inline constexpr int32_t kStatusCodecX = 196;
inline lv_color_t status_border() { return lv_color_hex(0x2A2050); }

// --- Now playing ---------------------------------------------------------
// Re-laid out for 320x240 rather than stretched from 320x170.  Cover and track
// text share the top band, the spectrum runs the full width below them, and the
// control bar is pinned to the bottom edge.
//
// The spectrum keeps upstream's grid exactly -- 28 columns by 11 rows.  Going
// full width is what buys the bigger cells (6x8 on an 8x10 pitch here against
// 5x3 on an 8x4 pitch upstream); the count is never traded for size.
inline constexpr int32_t kCoverX = 8;
inline constexpr int32_t kCoverY = 24;
inline constexpr int32_t kCoverSize = 72;

inline constexpr int32_t kTextColumnX = 88;
inline constexpr int32_t kTextColumnWidth = 224;
inline constexpr int32_t kTitleY = 26;
inline constexpr int32_t kLyricY = 50;
// Elapsed and total share one row beside the cover, at either end of the text
// column.
inline constexpr int32_t kTimeRowY = 76;

inline constexpr int32_t kSpectrumCols = 28;
inline constexpr int32_t kSpectrumRows = 11;
inline constexpr int32_t kSpectrumColPitch = 10;
inline constexpr int32_t kSpectrumRowPitch = 7;
inline constexpr int32_t kSpectrumCellWidth = 8;
inline constexpr int32_t kSpectrumCellHeight = 5;
inline constexpr int32_t kSpectrumWidth = kSpectrumCols * kSpectrumColPitch;
inline constexpr int32_t kSpectrumHeight = kSpectrumRows * kSpectrumRowPitch - 1;
inline constexpr int32_t kSpectrumX = (kScreenWidth - kSpectrumWidth) / 2;
inline constexpr int32_t kSpectrumY = 98;

// Seek slider spans the full content width under the spectrum.
inline constexpr int32_t kProgressX = 8;
inline constexpr int32_t kProgressWidth = kScreenWidth - 2 * kProgressX;
inline constexpr int32_t kProgressY = 182;
inline constexpr int32_t kProgressHeight = 12;

// Control bar: seven 45px slots, pinned to the bottom edge.  The play button
// sits in slot 2 and is deliberately NOT centred on the screen -- upstream
// tried centring it and the change was rejected; the order is fixed.
inline constexpr int32_t kControlBarHeight = 36;
inline constexpr int32_t kControlBarY = kScreenHeight - kControlBarHeight;
inline constexpr int32_t kControlSlots = 7;
inline constexpr int32_t kControlSlotWidth = kScreenWidth / kControlSlots;
inline constexpr int32_t kPlayRingSize = 32;

// App exit affordance.  The list page has no status bar, so "leave the app"
// lives in the tab row; the left-edge swipe does the same thing, matching the
// gesture the player page already uses for back.
inline constexpr int32_t kExitWidth = 34;
inline constexpr int32_t kExitHeight = 20;
inline constexpr int32_t kExitRight = -6;
inline constexpr int32_t kExitY = 4;
// The track count moves left to make room for the exit button.
inline constexpr int32_t kCountRight = -46;

}  // namespace hifi_theme
