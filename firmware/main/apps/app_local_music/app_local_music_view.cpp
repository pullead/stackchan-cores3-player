// LOCAL MUSIC 的界面。
//
// 改写自 LVGL 官方音乐播放器 demo（demos/music/lv_demo_music_main.c 与
// lv_demo_music_list.c），原作 Copyright (c) LVGL Kft，MIT 许可：
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions: The above copyright
//   notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.  THE SOFTWARE IS PROVIDED "AS IS",
//   WITHOUT WARRANTY OF ANY KIND.
//
// 相对原 demo 的改动：
// - 尺寸从 480x272 换算到 CoreS3 的 320x240，图片按新布局重新生成
//   （music_assets/generate.py）；
// - 全局变量改为 AppLocalMusic 成员，事件只记录待执行动作；
// - 环形频谱改用实时 FFT（SpectrumAnalyzer），不再播放预录的频谱表；
// - 曲目列表改为固定对象池 + 按滚动位置重绑，1000 首也只有 7 行控件；
// - 去掉片头动画；中文用固件自带的 font_puhui_14_1；
// - 白底与波浪装饰换成按专辑封面生成的毛玻璃背景，配色随封面切换，封面
//   裁成圆形放在频谱中央。

#include "app_local_music.h"

#include <hal/hal.h>
#include <media/library/cover_decoder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

LV_FONT_DECLARE(font_puhui_14_1);

extern "C" {
LV_IMAGE_DECLARE(lm_btn_play);
LV_IMAGE_DECLARE(lm_btn_pause);
LV_IMAGE_DECLARE(lm_btn_prev);
LV_IMAGE_DECLARE(lm_btn_next);
LV_IMAGE_DECLARE(lm_btn_rnd);
LV_IMAGE_DECLARE(lm_btn_loop);
LV_IMAGE_DECLARE(lm_btn_list_play);
LV_IMAGE_DECLARE(lm_btn_list_pause);
LV_IMAGE_DECLARE(lm_icn_heart);
LV_IMAGE_DECLARE(lm_list_border);
LV_IMAGE_DECLARE(lm_cover_default);
}

namespace {

constexpr int32_t kScreenWidth = 320;
constexpr int32_t kScreenHeight = 240;
// 停在列表上时，播放页还露出这么高的一条：既显示正在播放的曲名，也是把
// 播放页拖回来的把手。列表从这条之下开始。
constexpr int32_t kStripHeight = 40;
constexpr int32_t kListScroll = kScreenHeight - kStripHeight;
constexpr int32_t kListHeaderHeight = 32;
constexpr int32_t kRowPitch = 40;
constexpr int32_t kLeftColumnWidth = 172;
constexpr int32_t kTextWidth = 150;
// 左边缘右滑：起点必须在这个宽度以内，再往里的滑动属于手指下面的控件。
constexpr int32_t kEdgeGestureWidth = 40;

// 环形频谱。demo 的封面 176px、静止半径 82；这里封面 108px，按同一比例。
constexpr int32_t kSpectrumSize = 156;
constexpr int32_t kBarRestRadius = 50;
constexpr int32_t kBarMaxRadius = kSpectrumSize / 2;
constexpr int32_t kBarColour1Stop = 49;
constexpr int32_t kBarColour2Stop = 61;
constexpr int32_t kBarColour3Stop = kBarMaxRadius;
constexpr int32_t kBarCount = 20;
constexpr int32_t kDegStep = 180 / kBarCount;
constexpr int32_t kBandCount = 4;
constexpr int32_t kBarsPerBand = kBarCount / kBandCount;
// demo 频谱表的量程约 0..80；SpectrumAnalyzer 的列是 0..255。
constexpr int32_t kBandScale = 80;
// 低音让封面放大的上限（LV_SCALE_NONE = 256 为原大）。
constexpr int32_t kCoverBoost = 24;
// 约 30 帧/秒，与 demo 频谱表的 33 帧/秒相当。
constexpr uint32_t kSpectrumIntervalMs = 33;

// 列表沿用 demo 的深色配色；播放页的颜色由封面决定（apply_theme）。
constexpr uint32_t kPlayerBg = 0xffffff;
constexpr uint32_t kInkMid = 0x8a86b8;
constexpr uint32_t kListBg = 0x343247;
constexpr uint32_t kListPressed = 0x4c4965;
constexpr uint32_t kListTitle = 0xffffff;
constexpr uint32_t kListArtist = 0xb1b0be;
constexpr uint32_t kFavourite = 0xe0457b;

lv_color_t to_lv(const media::Rgb& colour) { return lv_color_make(colour.r, colour.g, colour.b); }

constexpr uint16_t kRnd[30] = {994, 285, 553, 11,  792, 707, 966, 641, 852, 827,
                               44,  352, 146, 581, 490, 80,  729, 58,  695, 940,
                               724, 561, 124, 653, 27,  292, 557, 506, 382, 199};

const lv_font_t* cjk_font() { return &font_puhui_14_1; }

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t colour, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    lv_label_set_text(label, text);
    return label;
}

// 单行打点。LV_LABEL_LONG_DOT 按宽度换行、超出高度才打点，所以高度要锁成一行。
void pin_single_line(lv_obj_t* label, int32_t width) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(label, width,
                    lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
}

lv_obj_t* make_box(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

int32_t get_cos(int32_t deg, int32_t a) {
    int32_t r = lv_trigo_cos(deg) * a;
    r += LV_TRIGO_SIN_MAX / 2;
    return r >> LV_TRIGO_SHIFT;
}

int32_t get_sin(int32_t deg, int32_t a) {
    const int32_t r = lv_trigo_sin(deg) * a;
    return (r + LV_TRIGO_SIN_MAX / 2) >> LV_TRIGO_SHIFT;
}

void format_clock(char* out, std::size_t size, uint32_t seconds) {
    std::snprintf(out, size, "%u:%02u", static_cast<unsigned>(seconds / 60),
                  static_cast<unsigned>(seconds % 60));
}

// 文件扩展名转大写，用作格式标签（MP3 / FLAC / AAC）。
std::string extension_label(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return "";
    std::string ext = path.substr(dot + 1);
    for (char& character : ext) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    }
    return ext;
}

}  // namespace

// ---------------------------------------------------------------------------
// 构建与销毁
// ---------------------------------------------------------------------------

void AppLocalMusic::build_ui() {
    destroy_ui();

    root_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kListBg), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(root_, this);

    // 与 demo 相同的叠放：列表在下，播放页在上并可整体上滑。
    build_list_panel();
    build_player_panel();

    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            gesture_indev_ = indev;
            break;
        }
    }
    if (gesture_indev_ != nullptr) {
        lv_indev_add_event_cb(gesture_indev_, on_indev_event, LV_EVENT_PRESSED, this);
        lv_indev_add_event_cb(gesture_indev_, on_indev_event, LV_EVENT_GESTURE, this);
    }

    shown_info_.clear();
    shown_time_.clear();
    shown_format_.clear();
    shown_state_ = media::PlaybackState::Idle;
    shown_list_visible_ = !list_visible();
    spectrum_resting_ = false;
    apply_theme();
    show_track_info();
    update_controls();
    reload_list();
}

void AppLocalMusic::destroy_ui() {
    if (gesture_indev_ != nullptr) {
        lv_indev_remove_event_cb_with_user_data(gesture_indev_, on_indev_event, this);
        gesture_indev_ = nullptr;
    }
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    // 下面这些都在刚删掉的树里；留着旧指针，下次重绑就会写进已释放的对象。
    list_panel_ = nullptr;
    list_ = nullptr;
    list_spacer_ = nullptr;
    list_empty_ = nullptr;
    count_label_ = nullptr;
    tab_labels_.fill(nullptr);
    main_cont_ = nullptr;
    player_ = nullptr;
    title_label_ = nullptr;
    artist_label_ = nullptr;
    info_label_ = nullptr;
    favourite_icon_ = nullptr;
    format_label_ = nullptr;
    background_ = nullptr;
    shuffle_icon_ = nullptr;
    prev_icon_ = nullptr;
    next_icon_ = nullptr;
    play_icon_ = nullptr;
    loop_icon_ = nullptr;
    loop_badge_ = nullptr;
    progress_ = nullptr;
    time_label_ = nullptr;
    spectrum_obj_ = nullptr;
    cover_ = nullptr;
    handle_label_ = nullptr;
    row_pool_.fill(nullptr);
    row_icon_.fill(nullptr);
    row_title_.fill(nullptr);
    row_artist_.fill(nullptr);
    row_position_.fill(0);
    first_row_ = -1;
}

void AppLocalMusic::build_list_panel() {
    list_panel_ = make_box(root_);
    lv_obj_set_pos(list_panel_, 0, kStripHeight);
    lv_obj_set_size(list_panel_, kScreenWidth, kScreenHeight - kStripHeight);
    lv_obj_set_style_bg_color(list_panel_, lv_color_hex(kListBg), 0);
    lv_obj_set_style_bg_opa(list_panel_, LV_OPA_COVER, 0);

    // 表头：两个视图标签、计数、回到桌面。
    static const char* kTabText[2] = {"全部歌曲", "★ 收藏"};
    static const int32_t kTabX[2] = {4, 84};
    for (int32_t i = 0; i < 2; ++i) {
        lv_obj_t* tab = make_box(list_panel_);
        lv_obj_set_pos(tab, kTabX[i], 0);
        lv_obj_set_size(tab, 76, kListHeaderHeight);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(tab, this);
        lv_obj_add_event_cb(tab, on_tab_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        tab_labels_[i] = make_label(tab, cjk_font(), kListArtist, kTabText[i]);
        lv_obj_center(tab_labels_[i]);
    }

    count_label_ = make_label(list_panel_, cjk_font(), kListArtist, "");
    lv_obj_align(count_label_, LV_ALIGN_TOP_RIGHT, -44, 8);

    // 唯一一个看得见的出口；左边缘右滑是另一个。
    lv_obj_t* home = make_box(list_panel_);
    lv_obj_set_size(home, 40, kListHeaderHeight);
    lv_obj_align(home, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(home, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(home, this);
    lv_obj_add_event_cb(home, on_action_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(PendingAction::Exit)));
    lv_obj_t* home_icon = make_label(home, &lv_font_montserrat_16, kListTitle, LV_SYMBOL_HOME);
    lv_obj_center(home_icon);

    lv_obj_t* divider = lv_image_create(list_panel_);
    lv_image_set_src(divider, &lm_list_border);
    lv_image_set_inner_align(divider, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_size(divider, kScreenWidth, lm_list_border.header.h);
    lv_obj_set_pos(divider, 0, kListHeaderHeight - lm_list_border.header.h);

    list_ = lv_obj_create(list_panel_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_pos(list_, 0, kListHeaderHeight);
    lv_obj_set_size(list_, kScreenWidth, kScreenHeight - kStripHeight - kListHeaderHeight);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_ACTIVE);
    // demo 的滚动条样式。
    lv_obj_set_style_width(list_, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list_, lv_color_hex3(0xeee), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list_, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(list_, 4, LV_PART_SCROLLBAR);
    lv_obj_set_user_data(list_, this);
    lv_obj_add_event_cb(list_, on_list_scrolled, LV_EVENT_SCROLL, nullptr);

    // 撑出完整内容高度：只有对象池的几行，容器几乎没有可滚的距离。
    list_spacer_ = make_box(list_);
    lv_obj_remove_flag(list_spacer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(list_spacer_, 0, 0);
    lv_obj_set_size(list_spacer_, 1, 1);

    for (int32_t slot = 0; slot < kRowPool; ++slot) {
        lv_obj_t* row = make_box(list_);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(row, kScreenWidth, kRowPitch);
        lv_obj_set_style_bg_color(row, lv_color_hex(kListPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, lv_color_hex(kListPressed), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_user_data(row, this);
        // 回调不带下标：行是对象池里的槽位，显示什么随滚动而变。
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* icon = lv_image_create(row);
        lv_image_set_src(icon, &lm_btn_list_play);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t* title = make_label(row, cjk_font(), kListTitle, "");
        pin_single_line(title, 260);
        lv_obj_set_pos(title, 46, 3);

        lv_obj_t* artist = make_label(row, cjk_font(), kListArtist, "");
        pin_single_line(artist, 260);
        lv_obj_set_pos(artist, 46, 21);

        lv_obj_t* border = lv_image_create(row);
        lv_image_set_src(border, &lm_list_border);
        lv_image_set_inner_align(border, LV_IMAGE_ALIGN_TILE);
        lv_obj_set_size(border, kScreenWidth, lm_list_border.header.h);
        lv_obj_align(border, LV_ALIGN_BOTTOM_MID, 0, 0);

        row_pool_[slot] = row;
        row_icon_[slot] = icon;
        row_title_[slot] = title;
        row_artist_[slot] = artist;
    }

    list_empty_ = make_label(list_panel_, cjk_font(), kListArtist, "");
    lv_obj_align(list_empty_, LV_ALIGN_CENTER, 0, kListHeaderHeight / 2);
    lv_obj_add_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
}

void AppLocalMusic::build_player_panel() {
    // 透明的滚动容器：播放页在其中上下滑动，两个占位块提供吸附位置。
    main_cont_ = lv_obj_create(root_);
    lv_obj_remove_style_all(main_cont_);
    lv_obj_remove_flag(main_cont_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(main_cont_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(main_cont_, kScreenWidth, kScreenHeight);
    lv_obj_set_scroll_dir(main_cont_, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(main_cont_, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(main_cont_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_user_data(main_cont_, this);
    lv_obj_add_event_cb(main_cont_, on_main_scroll_end, LV_EVENT_SCROLL_END, nullptr);

    player_ = lv_obj_create(main_cont_);
    lv_obj_remove_style_all(player_);
    lv_obj_set_pos(player_, 0, 0);
    lv_obj_set_size(player_, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_color(player_, lv_color_hex(kPlayerBg), 0);
    // 有毛玻璃背景图时它盖满整页，底色只会被白白填充一遍。
    lv_obj_set_style_bg_opa(player_, background_pixels_ != nullptr ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_remove_flag(player_, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_remove_flag(player_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(player_, this);

    // 占位块不可点击：停在列表上时，触摸要穿过它们落到下面的列表。
    lv_obj_t* stop_player = make_box(main_cont_);
    lv_obj_remove_flag(stop_player, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(stop_player, 0, 0);
    lv_obj_set_size(stop_player, kScreenWidth, kScreenHeight);
    lv_obj_t* stop_list = make_box(main_cont_);
    lv_obj_remove_flag(stop_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(stop_list, 0, kListScroll);
    lv_obj_set_size(stop_list, kScreenWidth, kScreenHeight);

    // 毛玻璃背景：一张全屏 RGB565 图，最先创建，垫在所有控件下面。缓冲区
    // 分配失败时退回 demo 的白底。
    if (background_pixels_ != nullptr) {
        background_ = lv_image_create(player_);
        lv_image_set_src(background_, &background_dsc_);
        lv_obj_set_pos(background_, 0, 0);
        lv_obj_add_flag(background_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    }

    // demo 横屏版的两栏：左栏文字与控制，右栏封面与环形频谱；最后一行是
    // 停在列表上时露出的那条。
    static const int32_t kColumns[] = {kLeftColumnWidth, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t kRows[] = {28,            LV_GRID_CONTENT, LV_GRID_FR(1),
                                    LV_GRID_CONTENT, LV_GRID_FR(1),   LV_GRID_CONTENT,
                                    LV_GRID_FR(1), kStripHeight,    LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(player_, kColumns, kRows);

    // 标题区。
    lv_obj_t* title_box = make_box(player_);
    lv_obj_set_height(title_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(title_box, 14, 0);
    lv_obj_set_style_pad_row(title_box, 2, 0);
    // 颜色在 apply_theme() 里按封面设置，这里只给占位色。
    title_label_ = make_label(title_box, cjk_font(), kInkMid, "");
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title_label_, kTextWidth);
    artist_label_ = make_label(title_box, cjk_font(), kInkMid, "");
    pin_single_line(artist_label_, kTextWidth);
    info_label_ = make_label(title_box, cjk_font(), kInkMid, "");
    pin_single_line(info_label_, kTextWidth);
    lv_obj_set_grid_cell(title_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    // 图标区：demo 这里是四个装饰图标，换成收藏与格式信息。
    lv_obj_t* icon_box = make_box(player_);
    lv_obj_set_height(icon_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icon_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(icon_box, 14, 0);
    lv_obj_set_style_pad_column(icon_box, 10, 0);
    favourite_icon_ = lv_image_create(icon_box);
    lv_image_set_src(favourite_icon_, &lm_icn_heart);
    lv_obj_add_flag(favourite_icon_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(favourite_icon_, 10);
    lv_obj_set_user_data(favourite_icon_, this);
    lv_obj_add_event_cb(favourite_icon_, on_action_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(PendingAction::ToggleFavourite)));
    format_label_ = make_label(icon_box, &lv_font_montserrat_12, kInkMid, "");
    lv_obj_set_grid_cell(icon_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 3, 1);

    // 控制区：随机、上一首、播放、下一首、循环，下一行进度条与时间。
    lv_obj_t* ctrl_box = make_box(player_);
    lv_obj_set_height(ctrl_box, LV_SIZE_CONTENT);
    lv_obj_remove_flag(ctrl_box, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    static const int32_t kCtrlColumns[] = {LV_GRID_FR(1), 22, 38, 48, 38, 22, LV_GRID_FR(1),
                                           LV_GRID_TEMPLATE_LAST};
    static const int32_t kCtrlRows[] = {LV_GRID_CONTENT, 26, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(ctrl_box, kCtrlColumns, kCtrlRows);

    auto make_button = [this, ctrl_box](const lv_image_dsc_t* src, PendingAction action,
                                        int32_t column) {
        lv_obj_t* image = lv_image_create(ctrl_box);
        lv_image_set_src(image, src);
        lv_obj_add_flag(image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(image, 6);
        lv_obj_set_user_data(image, this);
        lv_obj_add_event_cb(image, on_action_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(action)));
        lv_obj_set_grid_cell(image, LV_GRID_ALIGN_CENTER, column, 1, LV_GRID_ALIGN_CENTER, 0, 1);
        return image;
    };
    shuffle_icon_ = make_button(&lm_btn_rnd, PendingAction::ToggleShuffle, 1);
    prev_icon_ = make_button(&lm_btn_prev, PendingAction::Prev, 2);
    play_icon_ = make_button(&lm_btn_play, PendingAction::PlayPause, 3);
    next_icon_ = make_button(&lm_btn_next, PendingAction::Next, 4);
    loop_icon_ = make_button(&lm_btn_loop, PendingAction::CycleRepeat, 5);
    // 单曲循环在循环键右上角标一个「1」。
    loop_badge_ = make_label(ctrl_box, &lv_font_montserrat_10, kInkMid, "1");
    lv_obj_set_grid_cell(loop_badge_, LV_GRID_ALIGN_END, 5, 1, LV_GRID_ALIGN_START, 0, 1);
    lv_obj_add_flag(loop_badge_, LV_OBJ_FLAG_HIDDEN);

    progress_ = lv_slider_create(ctrl_box);
    lv_slider_set_range(progress_, 0, 1000);
    lv_obj_set_height(progress_, 3);
    lv_obj_set_style_anim_duration(progress_, 100, 0);
    lv_obj_remove_flag(progress_, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_ext_click_area(progress_, 10);
    // 参考图的极简进度条：细轨道、强调色进度、小圆点拖钮。颜色见 apply_theme()。
    lv_obj_set_style_bg_opa(progress_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(progress_, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(progress_, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(progress_, 0, LV_PART_KNOB);
    lv_obj_set_style_outline_width(progress_, 0, 0);
    lv_obj_set_user_data(progress_, this);
    lv_obj_add_event_cb(progress_, on_progress_event, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(progress_, on_progress_event, LV_EVENT_RELEASED, nullptr);
    lv_obj_set_grid_cell(progress_, LV_GRID_ALIGN_STRETCH, 1, 3, LV_GRID_ALIGN_CENTER, 1, 1);

    time_label_ = make_label(ctrl_box, &lv_font_montserrat_12, kInkMid, "0:00");
    lv_obj_set_grid_cell(time_label_, LV_GRID_ALIGN_END, 4, 2, LV_GRID_ALIGN_CENTER, 1, 1);
    lv_obj_set_grid_cell(ctrl_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 5, 1);

    // 底部把手：点一下在播放页与列表之间切换。
    lv_obj_t* handle_box = make_box(player_);
    lv_obj_add_flag(handle_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(handle_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(handle_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(handle_box, 4, 0);
    lv_obj_set_user_data(handle_box, this);
    lv_obj_add_event_cb(handle_box, on_action_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(PendingAction::ToggleView)));
    handle_label_ = make_label(handle_box, cjk_font(), kInkMid, "全部曲目");
    pin_single_line(handle_label_, 280);
    lv_obj_set_style_text_align(handle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* handle_bar = make_box(handle_box);
    lv_obj_set_size(handle_bar, 20, 2);
    lv_obj_set_style_bg_color(handle_bar, lv_color_hex(kInkMid), 0);
    lv_obj_set_style_bg_opa(handle_bar, LV_OPA_COVER, 0);
    lv_obj_set_grid_cell(handle_box, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 7, 1);

    // 环形频谱与封面。最后创建，画在其他控件之上，与 demo 一致。
    spectrum_obj_ = make_box(player_);
    lv_obj_remove_flag(spectrum_obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(spectrum_obj_, kSpectrumSize, kSpectrumSize);
    lv_obj_set_user_data(spectrum_obj_, this);
    lv_obj_add_event_cb(spectrum_obj_, on_spectrum_draw, LV_EVENT_COVER_CHECK, nullptr);
    lv_obj_add_event_cb(spectrum_obj_, on_spectrum_draw, LV_EVENT_DRAW_MAIN_BEGIN, nullptr);
    lv_obj_set_grid_cell(spectrum_obj_, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 7);

    cover_ = lv_image_create(spectrum_obj_);
    // 有封面缓冲区时用生成的圆形封面，否则退回 demo 的方形默认图。
    if (cover_pixels_ != nullptr) {
        lv_image_set_src(cover_, &cover_dsc_);
    } else {
        lv_image_set_src(cover_, &lm_cover_default);
    }
    lv_image_set_antialias(cover_, false);
    lv_obj_center(cover_);
    lv_obj_add_flag(cover_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cover_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_user_data(cover_, this);
    lv_obj_add_event_cb(cover_, on_cover_gesture, LV_EVENT_GESTURE, nullptr);
}

// ---------------------------------------------------------------------------
// 列表
// ---------------------------------------------------------------------------

void AppLocalMusic::reload_list() {
    if (list_ == nullptr) return;
    for (std::size_t i = 0; i < tab_labels_.size(); ++i) {
        const bool selected = static_cast<std::size_t>(tab_) == i;
        lv_obj_set_style_text_color(tab_labels_[i],
                                    lv_color_hex(selected ? kListTitle : kInkMid), 0);
    }
    char header[32] = {};
    std::snprintf(header, sizeof(header), tab_ == Tab::Favourites ? "收藏 %u 首" : "共 %u 首",
                  static_cast<unsigned>(visible_.size()));
    lv_label_set_text(count_label_, header);

    const int32_t content = static_cast<int32_t>(visible_.size()) * kRowPitch;
    lv_obj_set_height(list_spacer_, content > 0 ? content : 1);
    if (visible_.empty()) {
        // 空的收藏不是错误，与卡上没有音乐的提示不同。
        lv_label_set_text(list_empty_, tab_ == Tab::Favourites ? "尚无收藏" : "未找到音乐文件");
        lv_obj_remove_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_scroll_to_y(list_, 0, LV_ANIM_OFF);
    rebind_rows(true);
}

void AppLocalMusic::rebind_rows(bool force) {
    if (list_ == nullptr || row_pool_[0] == nullptr) {
        return;
    }
    if (visible_.empty()) {
        for (lv_obj_t* row : row_pool_) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        first_row_ = -1;
        return;
    }
    const int32_t scroll_y = lv_obj_get_scroll_y(list_);
    int32_t first = scroll_y / kRowPitch;
    const int32_t last_first = static_cast<int32_t>(visible_.size()) - kRowPool;
    if (first > last_first) first = last_first;
    if (first < 0) first = 0;
    // Most scroll events do not move a whole row, and rebinding is the only
    // work this handler does.
    if (!force && first == first_row_) return;
    first_row_ = first;

    const bool playing =
        playback_ && playback_->tick().state == media::PlaybackState::Playing;
    for (int32_t slot = 0; slot < kRowPool; ++slot) {
        lv_obj_t* row = row_pool_[slot];
        const std::size_t position = static_cast<std::size_t>(first + slot);
        if (position >= visible_.size()) {
            // Fewer rows than pool slots: keep the surplus rows out of the way.
            row_position_[slot] = 0;
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        // Rows are laid out by their position in the view, not by track index,
        // so a filtered view has no gaps where the hidden tracks would be.
        const std::size_t index = visible_[position];
        row_position_[slot] = position;
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(row, 0, static_cast<int32_t>(position) * kRowPitch);
        lv_label_set_text(row_title_[slot], row_title_for(index));
        lv_label_set_text(row_artist_[slot], row_artist_for(index));
        const bool current = has_track_ && index == current_index_;
        if (current) {
            lv_obj_add_state(row, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(row, LV_STATE_CHECKED);
        }
        lv_image_set_src(row_icon_[slot],
                         current && playing ? &lm_btn_list_pause : &lm_btn_list_play);
    }
    shown_highlight_ = current_index_;
    shown_highlight_playing_ = playing;
}

// ---------------------------------------------------------------------------
// 播放页
// ---------------------------------------------------------------------------

void AppLocalMusic::show_track_info() {
    if (title_label_ == nullptr) return;
    if (!has_track_) {
        lv_label_set_text(title_label_, "未选择曲目");
        lv_label_set_text(artist_label_, "从列表中选择歌曲");
        return;
    }
    lv_label_set_text(title_label_, selected_title_.c_str());
    lv_label_set_text(artist_label_,
                      selected_artist_.empty() ? "未知艺术家" : selected_artist_.c_str());
    shown_list_visible_ = !list_visible();  // 让底部把手跟着换曲名
}

void AppLocalMusic::update_controls() {
    if (play_icon_ == nullptr) return;
    const bool playing =
        playback_ && playback_->tick().state == media::PlaybackState::Playing;
    lv_image_set_src(play_icon_, playing ? &lm_btn_pause : &lm_btn_play);

    // 未启用的模式键调暗，启用的全亮。
    lv_obj_set_style_image_opa(shuffle_icon_,
                               play_mode_ == PlayMode::Shuffle ? LV_OPA_COVER : LV_OPA_40, 0);
    const bool repeating = play_mode_ == PlayMode::RepeatAll || play_mode_ == PlayMode::RepeatOne;
    lv_obj_set_style_image_opa(loop_icon_, repeating ? LV_OPA_COVER : LV_OPA_40, 0);
    if (play_mode_ == PlayMode::RepeatOne) {
        lv_obj_remove_flag(loop_badge_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(loop_badge_, LV_OBJ_FLAG_HIDDEN);
    }

    // 收藏用颜色和透明度双重表达，不只靠颜色。
    const bool favourite = has_track_ && current_favourite_;
    lv_obj_set_style_image_opa(favourite_icon_, favourite ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_image_recolor(favourite_icon_,
                                   favourite ? lv_color_hex(kFavourite) : to_lv(theme_.ink), 0);
    lv_obj_set_style_image_recolor_opa(favourite_icon_, LV_OPA_COVER, 0);
}

void AppLocalMusic::apply_theme() {
    if (player_ == nullptr) return;
    const lv_color_t ink = to_lv(theme_.ink);
    const lv_color_t soft = to_lv(theme_.ink_soft);
    const lv_color_t faint = to_lv(theme_.ink_faint);
    const lv_color_t accent = to_lv(theme_.accent);

    lv_obj_set_style_text_color(title_label_, ink, 0);
    lv_obj_set_style_text_color(artist_label_, soft, 0);
    lv_obj_set_style_text_color(info_label_, faint, 0);
    lv_obj_set_style_text_color(format_label_, faint, 0);
    lv_obj_set_style_text_color(time_label_, soft, 0);
    lv_obj_set_style_text_color(handle_label_, soft, 0);
    lv_obj_set_style_text_color(loop_badge_, accent, 0);
    lv_obj_t* handle_bar = lv_obj_get_child(lv_obj_get_parent(handle_label_), 1);
    if (handle_bar != nullptr) lv_obj_set_style_bg_color(handle_bar, soft, 0);

    // demo 的按钮图是紫色渐变；整体重新着色成主题墨色，做成参考图那样的
    // 单色线性图标，深浅背景上都清楚。
    for (lv_obj_t* icon : {shuffle_icon_, prev_icon_, play_icon_, next_icon_, loop_icon_}) {
        lv_obj_set_style_image_recolor(icon, ink, 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }

    // 进度条：半透明墨色轨道，强调色进度与拖钮。
    lv_obj_set_style_bg_color(progress_, ink, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_, accent, LV_PART_KNOB);

    // 频谱光条：短条是强调色的淡色，中段是强调色，伸到最长时变白。
    const lv_color_t pale = theme_.dark ? lv_color_mix(accent, lv_color_black(), 160)
                                        : lv_color_mix(accent, lv_color_white(), 110);
    bar_colours_ = {pale, accent, lv_color_white()};

    update_controls();
    if (spectrum_obj_ != nullptr) lv_obj_invalidate(spectrum_obj_);
}

// ---------------------------------------------------------------------------
// 封面与毛玻璃背景
// ---------------------------------------------------------------------------

namespace {

void* alloc_psram(std::size_t bytes) {
#ifdef ESP_PLATFORM
    void* pointer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return pointer != nullptr ? pointer : lv_malloc(bytes);
#else
    return lv_malloc(bytes);
#endif
}

void free_psram(void* pointer) {
    if (pointer == nullptr) return;
#ifdef ESP_PLATFORM
    heap_caps_free(pointer);
#else
    lv_free(pointer);
#endif
}

}  // namespace

bool AppLocalMusic::allocate_cover_buffers() {
    if (cover_pixels_ != nullptr) return true;
    const std::size_t cover_bytes = static_cast<std::size_t>(kCoverPixels) * kCoverPixels * 4;
    const std::size_t background_bytes =
        static_cast<std::size_t>(kBackgroundWidth) * kBackgroundHeight * sizeof(uint16_t);
    cover_pixels_ = static_cast<uint8_t*>(alloc_psram(cover_bytes));
    cover_scratch_ = static_cast<uint8_t*>(alloc_psram(cover_bytes));
    background_pixels_ = static_cast<uint16_t*>(alloc_psram(background_bytes));
    background_scratch_ = static_cast<uint16_t*>(alloc_psram(background_bytes));
    if (cover_pixels_ == nullptr || cover_scratch_ == nullptr || background_pixels_ == nullptr ||
        background_scratch_ == nullptr) {
        free_cover_buffers();
        return false;
    }
    std::memset(cover_pixels_, 0, cover_bytes);
    std::memset(background_pixels_, 0xff, background_bytes);

    cover_dsc_ = {};
    cover_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    cover_dsc_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    cover_dsc_.header.w = kCoverPixels;
    cover_dsc_.header.h = kCoverPixels;
    cover_dsc_.header.stride = kCoverPixels * 4;
    cover_dsc_.data_size = cover_bytes;
    cover_dsc_.data = cover_pixels_;

    background_dsc_ = {};
    background_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    background_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    background_dsc_.header.w = kBackgroundWidth;
    background_dsc_.header.h = kBackgroundHeight;
    background_dsc_.header.stride = kBackgroundWidth * 2;
    background_dsc_.data_size = background_bytes;
    background_dsc_.data = reinterpret_cast<const uint8_t*>(background_pixels_);
    return true;
}

void AppLocalMusic::free_cover_buffers() {
    free_psram(cover_pixels_);
    free_psram(cover_scratch_);
    free_psram(background_pixels_);
    free_psram(background_scratch_);
    cover_pixels_ = nullptr;
    cover_scratch_ = nullptr;
    background_pixels_ = nullptr;
    background_scratch_ = nullptr;
    cover_dsc_ = {};
    background_dsc_ = {};
}

void AppLocalMusic::prepare_default_cover() {
    // demo 的默认封面是 RGB565，转成 RGB888 给封面加工流程用；它高 107 像素，
    // 最后一行重复一次补成正方形。
    const lv_image_header_t& header = lm_cover_default.header;
    const int size = kCoverPixels;
    default_cover_rgb_.assign(static_cast<std::size_t>(size) * size * 3, 0);
    for (int y = 0; y < size; ++y) {
        const int source_y = std::min<int>(y, header.h - 1);
        const auto* row = reinterpret_cast<const uint16_t*>(lm_cover_default.data +
                                                            static_cast<std::size_t>(source_y) * header.stride);
        for (int x = 0; x < size; ++x) {
            const uint16_t pixel = row[std::min<int>(x, header.w - 1)];
            uint8_t* out = default_cover_rgb_.data() + (static_cast<std::size_t>(y) * size + x) * 3;
            out[0] = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
            out[1] = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
            out[2] = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
        }
    }
}

void AppLocalMusic::load_cover(const uint8_t* jpeg, std::size_t length) {
    if (cover_pixels_ == nullptr) return;

    std::vector<uint8_t> decoded;
    const std::vector<uint8_t>* rgb = &default_cover_rgb_;
    if (jpeg != nullptr && media::decode_cover_jpeg(jpeg, length, kCoverPixels, decoded)) {
        rgb = &decoded;
    }
    if (rgb->size() != static_cast<std::size_t>(kCoverPixels) * kCoverPixels * 3) return;

    // 全部计算在锁外进行：背景放大是 7.7 万像素的运算，不能卡住界面。
    uint8_t grid[kThemeGrid * kThemeGrid * 3];
    media::downsample_rgb888(rgb->data(), kCoverPixels, kCoverPixels, grid, kThemeGrid, kThemeGrid);
    const media::CoverTheme theme = media::analyse_cover(grid, kThemeGrid, kThemeGrid);
    media::make_round_cover(rgb->data(), kCoverPixels, cover_scratch_);
    media::make_frosted_background(grid, kThemeGrid, kThemeGrid, theme, background_scratch_,
                                   kBackgroundWidth, kBackgroundHeight);

    LvglLockGuard lock;
    std::memcpy(cover_pixels_, cover_scratch_, cover_dsc_.data_size);
    std::memcpy(background_pixels_, background_scratch_, background_dsc_.data_size);
    // 像素换了但描述符地址没变。本固件的图像缓存大小为 0（CONFIG_LV_CACHE_DEF_SIZE），
    // 不存在旧图缓存；重新设一次源并整页重绘即可。
    theme_ = theme;
    if (cover_ != nullptr && cover_pixels_ != nullptr) lv_image_set_src(cover_, &cover_dsc_);
    if (background_ != nullptr) lv_image_set_src(background_, &background_dsc_);
    if (player_ != nullptr) {
        apply_theme();
        lv_obj_invalidate(player_);
    }
}

void AppLocalMusic::refresh_view() {
    if (root_ == nullptr) return;

    // tick() is lock-free: taking the controller mutex here would mean waiting
    // for the audio task to finish a whole chunk, so the clock would advance in
    // multi-second jumps instead of smoothly.
    const media::LocalPlaybackController::PlaybackTick tick =
        playback_ ? playback_->tick() : media::LocalPlaybackController::PlaybackTick{};

    const std::string info = playback_status(tick.state);
    if (info != shown_info_) {
        shown_info_ = info;
        lv_label_set_text(info_label_, shown_info_.c_str());
    }

    const uint32_t elapsed =
        tick.sample_rate > 0 ? static_cast<uint32_t>(tick.played_frames / tick.sample_rate) : 0;
    char clock[32] = {};
    char total[16] = {};
    format_clock(clock, sizeof(clock), has_track_ ? elapsed : 0);
    if (total_seconds_ > 0) {
        format_clock(total, sizeof(total), total_seconds_);
        std::strncat(clock, " / ", sizeof(clock) - std::strlen(clock) - 1);
        std::strncat(clock, total, sizeof(clock) - std::strlen(clock) - 1);
    }
    if (shown_time_ != clock) {
        shown_time_ = clock;
        lv_label_set_text(time_label_, clock);
        if (!seeking_) {
            // Driven by the same tick as the clock, so the bar and the label
            // cannot disagree; left alone while a finger is on it.
            int32_t value = 0;
            if (total_seconds_ > 0) {
                const uint32_t clamped = elapsed > total_seconds_ ? total_seconds_ : elapsed;
                value = static_cast<int32_t>(static_cast<int64_t>(clamped) * 1000 / total_seconds_);
            }
            lv_slider_set_value(progress_, value, LV_ANIM_OFF);
        }
    }

    std::string format;
    if (has_track_ && tick.sample_rate > 0 && current_index_ < tracks_.size()) {
        char rate[24] = {};
        std::snprintf(rate, sizeof(rate), "  %u.%u kHz", static_cast<unsigned>(tick.sample_rate / 1000),
                      static_cast<unsigned>((tick.sample_rate % 1000) / 100));
        format = extension_label(tracks_[current_index_].path) + rate;
    }
    if (format != shown_format_) {
        shown_format_ = format;
        lv_label_set_text(format_label_, shown_format_.c_str());
    }

    if (tick.state != shown_state_) {
        shown_state_ = tick.state;
        update_controls();
    }
    const bool playing = tick.state == media::PlaybackState::Playing;
    if (shown_highlight_ != current_index_ || shown_highlight_playing_ != playing) {
        rebind_rows(true);
    }

    // 底部把手：在播放页上是「全部曲目」，停在列表上时显示正在播放的曲名。
    const bool on_list = list_visible();
    if (on_list != shown_list_visible_) {
        shown_list_visible_ = on_list;
        if (!on_list) {
            lv_label_set_text(handle_label_, "全部曲目");
        } else if (has_track_) {
            const std::string text = "正在播放：" + selected_title_;
            lv_label_set_text(handle_label_, text.c_str());
        } else {
            lv_label_set_text(handle_label_, "未在播放");
        }
    }

    refresh_spectrum();
}

void AppLocalMusic::refresh_spectrum() {
    if (spectrum_obj_ == nullptr || cover_ == nullptr) return;
    const uint32_t now = lv_tick_get();
    if (now - last_spectrum_tick_ < kSpectrumIntervalMs) return;
    // 手指正在拖动滚动时不更新频谱：每帧失效 156x156 的区域会和滚动抢渲染
    // 时间，滚动是用户直接感知的，优先保证它跟手。
    if (gesture_indev_ != nullptr && lv_indev_get_scroll_obj(gesture_indev_) != nullptr) return;
    last_spectrum_tick_ = now;

    // Drain whatever the audio task published since the last frame.  The FFT
    // runs here, on the UI side, never in the audio writer.  The tap carries
    // interleaved samples, so the folding has to use the stream's real channel
    // count.
    const auto tick = playback_ ? playback_->tick() : media::LocalPlaybackController::PlaybackTick{};
    const uint8_t channels = tick.channels > 0 ? tick.channels : 1;
    int16_t samples[256];
    bool fed = false;
    while (true) {
        const std::size_t count = pcm_tap_.pop(samples, sizeof(samples) / sizeof(samples[0]));
        if (count == 0) break;
        analyzer_.push(samples, count, channels);
        fed = true;
    }
    if (!fed) {
        analyzer_.decay();
    }

    // 28 列对数分布的频谱合成 demo 用的 4 个频段。
    static constexpr int32_t kBandEdges[kBandCount + 1] = {0, 4, 10, 18,
                                                           media::SpectrumAnalyzer::kColumns};
    const auto& columns = analyzer_.columns();
    bool any = false;
    for (int32_t band = 0; band < kBandCount; ++band) {
        int32_t sum = 0;
        for (int32_t c = kBandEdges[band]; c < kBandEdges[band + 1]; ++c) {
            sum += static_cast<int32_t>(columns[c]);
        }
        const int32_t width = kBandEdges[band + 1] - kBandEdges[band];
        bands_[band] = sum / width * kBandScale / 255;
        any = any || bands_[band] > 0;
    }

    const bool playing = tick.state == media::PlaybackState::Playing;
    if (!any && !playing) {
        // 静止后只画一次，之后不再每帧失效整块区域。
        if (!spectrum_resting_) {
            spectrum_resting_ = true;
            lv_image_set_scale(cover_, LV_SCALE_NONE);
            lv_obj_invalidate(spectrum_obj_);
        }
        return;
    }
    spectrum_resting_ = false;
    ++spectrum_frame_;

    // demo 的节奏效果：重低音累计两次让光条整体错位，无低音时缓慢转动。
    if (bands_[0] > 12) {
        if (spectrum_frame_ - last_bass_frame_ > 5) {
            ++bass_count_;
            last_bass_frame_ = spectrum_frame_;
            if (bass_count_ >= 2) {
                bass_count_ = 0;
                ++bar_ofs_;
            }
        }
    }
    if (bands_[0] < 4) ++bar_rot_;

    const int32_t bass = std::min(bands_[0], kBandScale);
    lv_image_set_scale(cover_, static_cast<uint32_t>(LV_SCALE_NONE + bass * kCoverBoost / kBandScale));
    lv_obj_invalidate(spectrum_obj_);
}

void AppLocalMusic::scroll_to_list(bool animate) {
    if (main_cont_ == nullptr) return;
    lv_obj_scroll_to_y(main_cont_, kListScroll, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

void AppLocalMusic::scroll_to_player(bool animate) {
    if (main_cont_ == nullptr) return;
    lv_obj_scroll_to_y(main_cont_, 0, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

bool AppLocalMusic::list_visible() const {
    return main_cont_ != nullptr && lv_obj_get_scroll_y(main_cont_) > kListScroll / 2;
}

// ---------------------------------------------------------------------------
// 事件
// ---------------------------------------------------------------------------

void AppLocalMusic::on_spectrum_draw(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_COVER_CHECK) {
        lv_event_set_cover_res(event, LV_COVER_RES_NOT_COVER);
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN_BEGIN) return;

    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(obj));
    if (self == nullptr) return;
    lv_layer_t* layer = lv_event_get_layer(event);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    lv_point_t center;
    center.x = coords.x1 + lv_obj_get_width(obj) / 2;
    center.y = coords.y1 + lv_obj_get_height(obj) / 2;

    lv_draw_triangle_dsc_t draw_dsc;
    lv_draw_triangle_dsc_init(&draw_dsc);
    draw_dsc.opa = LV_OPA_COVER;

    int32_t radius[kBarCount];
    for (int32_t i = 0; i < kBarCount; ++i) radius[i] = kBarRestRadius;

    // 每个频段在主光条两侧按余弦衰减加「旁条」，低频段铺得更宽。
    static constexpr int32_t kBandWidth[kBandCount] = {20, 8, 4, 2};
    for (int32_t band = 0; band < kBandCount; ++band) {
        const int32_t width = kBandWidth[band];
        for (int32_t f = 0; f < width; ++f) {
            const int32_t amplitude = self->bands_[band];
            const int32_t modulation = get_cos(f * 360 / width + 180, 180) + 180;
            int32_t t = kBarsPerBand * band - width / 2 + f;
            if (t < 0) t += kBarCount;
            if (t >= kBarCount) t -= kBarCount;
            // demo 是 >> 9；尺寸缩到约 0.62 倍，这里多乘 5/8。
            radius[t] += (amplitude * modulation * 5) >> 12;
        }
    }

    // 换歌后的前 20 帧从旧排布过渡到新排布。
    constexpr int32_t kBlendFrames = 20;
    const int32_t blend = static_cast<int32_t>(
        std::min<uint32_t>(self->spectrum_frame_, static_cast<uint32_t>(kBlendFrames)));
    for (int32_t i = 0; i < kBarCount; ++i) {
        const int32_t j = (i + self->bar_rot_ + kRnd[self->bar_ofs_ % 10]) % kBarCount;
        const int32_t k = (i + self->bar_rot_ + kRnd[(self->bar_ofs_ + 1) % 10]) % kBarCount;
        int32_t v = (radius[k] * blend + radius[j] * (kBlendFrames - blend)) / kBlendFrames;
        if (v > kBarMaxRadius) v = kBarMaxRadius;

        const auto& colours = self->bar_colours_;
        if (v < kBarColour1Stop) {
            draw_dsc.color = colours[0];
        } else if (v >= kBarColour3Stop) {
            draw_dsc.color = colours[2];
        } else if (v > kBarColour2Stop) {
            draw_dsc.color = lv_color_mix(colours[2], colours[1],
                                          static_cast<uint8_t>((v - kBarColour2Stop) * 255 /
                                                               (kBarColour3Stop - kBarColour2Stop)));
        } else {
            draw_dsc.color = lv_color_mix(colours[1], colours[0],
                                          static_cast<uint8_t>((v - kBarColour1Stop) * 255 /
                                                               (kBarColour2Stop - kBarColour1Stop)));
        }

        constexpr int32_t kDegSpace = 1;
        int32_t deg = i * kDegStep + 90 + kDegSpace;
        const int32_t x1_out = get_cos(deg, v);
        draw_dsc.p[0].x = center.x + x1_out;
        draw_dsc.p[0].y = center.y + get_sin(deg, v);
        deg += kDegStep - kDegSpace * 2;
        const int32_t x2_out = get_cos(deg, v);
        draw_dsc.p[1].x = center.x + x2_out;
        draw_dsc.p[1].y = center.y + get_sin(deg, v);
        const int32_t x2_in = get_cos(deg, 0);
        draw_dsc.p[2].x = center.x + x2_in;
        draw_dsc.p[2].y = center.y + get_sin(deg, 0);
        lv_draw_triangle(layer, &draw_dsc);

        // 左右镜像。
        draw_dsc.p[0].x = center.x - x1_out;
        draw_dsc.p[1].x = center.x - x2_out;
        draw_dsc.p[2].x = center.x - x2_in;
        lv_draw_triangle(layer, &draw_dsc);
    }
}

void AppLocalMusic::on_row_clicked(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    // The row is a pool slot, so the track it stands for is whatever the last
    // rebind put there: a position in the current view, not a track index.
    for (int32_t slot = 0; slot < kRowPool; ++slot) {
        if (self->row_pool_[slot] == target) {
            const std::size_t position = self->row_position_[slot];
            if (position >= self->visible_.size()) return;
            self->pending_index_ = self->visible_[position];
            self->pending_action_ = PendingAction::SelectTrack;
            return;
        }
    }
}

void AppLocalMusic::on_list_scrolled(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    self->rebind_rows(false);
}

void AppLocalMusic::on_action_clicked(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    self->pending_action_ =
        static_cast<PendingAction>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void AppLocalMusic::on_tab_clicked(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    const auto index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    self->pending_tab_ = index == 0 ? Tab::Songs : Tab::Favourites;
    self->pending_action_ = PendingAction::SwitchTab;
}

void AppLocalMusic::on_progress_event(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(slider));
    if (self == nullptr) return;

    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        self->seeking_ = true;
        return;
    }

    // Only on release: seeking on every value change would issue dozens of seeks
    // and decoder re-opens during one drag.
    self->seeking_ = false;
    const int32_t range = lv_slider_get_max_value(slider);
    if (range <= 0) return;
    self->pending_seek_fraction_ =
        static_cast<float>(lv_slider_get_value(slider)) / static_cast<float>(range);
    self->pending_action_ = PendingAction::Seek;
}

void AppLocalMusic::on_cover_gesture(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    lv_indev_t* indev = lv_indev_active();
    if (self == nullptr || indev == nullptr) return;
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) self->pending_action_ = PendingAction::Next;
    if (dir == LV_DIR_RIGHT) self->pending_action_ = PendingAction::Prev;
}

void AppLocalMusic::on_main_scroll_end(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    // 只让下一次刷新重新判断把手文字；真正的更新在 refresh_view() 里。
    self->shown_list_visible_ = !self->list_visible();
}

void AppLocalMusic::on_indev_event(lv_event_t* event) {
    auto* self = static_cast<AppLocalMusic*>(lv_event_get_user_data(event));
    lv_indev_t* indev = lv_indev_active();
    if (self == nullptr || indev == nullptr || self->root_ == nullptr) return;

    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        lv_point_t point{};
        lv_indev_get_point(indev, &point);
        self->press_start_x_ = point.x;
        return;
    }
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;
    // Edge gesture only: a swipe that starts mid-screen belongs to whatever is
    // under the finger, not to navigation.
    if (self->press_start_x_ > kEdgeGestureWidth) return;
    // 播放页上回到列表；列表上退出应用。
    self->pending_action_ = self->list_visible() ? PendingAction::Exit : PendingAction::ShowList;
}
