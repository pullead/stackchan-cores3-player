/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <cstdint>

using namespace mooncake;

void AppLauncher::onLauncherCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");

    // 打开自己
    open();
}

void AppLauncher::onLauncherOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    LvglLockGuard lock;

    if (!_startup_checked && !GetHAL().isAppConfiged()) {
        mclog::tagInfo(getAppInfo().name, "app not configured, start startup worker");
        _startup_worker = std::make_unique<setup_workers::StartupWorker>();
    } else {
        create_launcher_view();
    }
}

void AppLauncher::onLauncherRunning()
{
    // 动态壁纸的 JPEG 解码约 20 ms，放在 LVGL 锁外，免得每帧卡住界面刷新；
    // 拿到锁之后只把解好的那块缓冲换上屏幕。
    const bool wallpaper_frame = _wallpaper && _wallpaper->decode_next(GetHAL().millis());

    LvglLockGuard lock;

    if (_startup_worker) {
        _startup_worker->update();
        if (_startup_worker->isDone()) {
            _startup_worker.reset();
            _startup_checked = true;
            create_launcher_view();
        }
    } else {
        _view->update();
        screensaver_update();
        if (wallpaper_frame && _wallpaper) {
            _wallpaper->present();
        }
    }

    GetStackChan().update();
}

void AppLauncher::onLauncherClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    LvglLockGuard lock;

    stop_screensaver();
    _view.reset();
}

void AppLauncher::onLauncherDestroy()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}

void AppLauncher::create_launcher_view()
{
    _view = std::make_unique<view::LauncherView>();
    _view->init(getAppProps());
    _view->onAppClicked = [&](int appID) {
        mclog::tagInfo(getAppInfo().name, "handle open app, app id: {}", appID);
        openApp(appID);
    };
}

void AppLauncher::screensaver_update()
{
    const uint32_t SCREENSAVER_TIMEOUT_MS = 30000;

    uint32_t idle_time = lv_display_get_inactive_time(NULL);
    if (idle_time >= SCREENSAVER_TIMEOUT_MS) {
        if (!_screensaver && !_wallpaper) {
            // 优先播放 ota_1 分区里的动态壁纸；没有或打不开就用原来的 DVD 屏保。
            if (view::WallpaperPlayer::available()) {
                auto wallpaper = std::make_unique<view::WallpaperPlayer>();
                if (wallpaper->open()) {
                    wallpaper->build_screen();
                    _wallpaper = std::move(wallpaper);
                }
            }
            if (!_wallpaper) {
                _screensaver = std::make_unique<view::Screensaver>();
                _screensaver->init();
            }
        }
    } else {
        // 触摸会清零空闲时间：任意一碰就退出屏保。
        stop_screensaver();
    }

    // Update in 30ms interval
    if (_screensaver && GetHAL().millis() - _screensaver_timecount > 30) {
        _screensaver_timecount = GetHAL().millis();
        _screensaver->update();
    }
}

void AppLauncher::stop_screensaver()
{
    // 调用方持有 LVGL 锁：两种屏保的析构都要把原来的屏幕换回来。
    if (_wallpaper) {
        _wallpaper->close_screen();
        _wallpaper.reset();
    }
    _screensaver.reset();
}
