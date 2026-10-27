#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <chrono>
#include <memory>
#include <string>

class DynamicGlyphCache;
class LvglFont;

class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) override;
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override;
    virtual void ClearTextGlyphs() override;
    virtual bool SetTextFont(std::shared_ptr<LvglFont> text_font) override;
    virtual bool SupportsGuiOperations() const override { return true; }
    virtual void ShowCountdown(int seconds, const char* text) override;
    virtual void HideCountdown() override;
    virtual bool IsCountdownVisible() const override { return countdown_visible_; }
    virtual void ShowMusic(const char* title, const char* artist, int duration_seconds,
                           int position_seconds, bool paused, const char* mode = nullptr) override;
    virtual void UpdateMusic(int position_seconds, int duration_seconds, bool paused) override;
    virtual void HideMusic() override;
    virtual bool IsMusicVisible() const override { return music_visible_; }

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t* display_ = nullptr;

    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;

    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;
    int last_displayed_clock_min_ = -1;   // -1 forces update on first idle tick
    std::unique_ptr<DynamicGlyphCache> dynamic_glyph_cache_;

    // 倒计时叠加层：全屏容器盖在最上层，表情和聊天气泡都在它下面，互不干扰。
    lv_obj_t* countdown_container_ = nullptr;
    lv_obj_t* countdown_num_label_ = nullptr;
    lv_obj_t* countdown_hint_label_ = nullptr;
    int countdown_remaining_ = 0;
    bool countdown_visible_ = false;  // 真正在显示吗；容器指针非空不代表看得见
    esp_timer_handle_t countdown_timer_ = nullptr;
    void CountdownTick();
    void UpdateCountdownLabel();

    // 音乐叠加层：5 根跳动的柱子 + 歌名/歌手 + 进度条 + 时间。
    lv_obj_t* music_container_ = nullptr;
    lv_obj_t* music_bars_row_ = nullptr;
    lv_obj_t* music_bars_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* music_title_label_ = nullptr;
    lv_obj_t* music_artist_label_ = nullptr;
    lv_obj_t* music_progress_ = nullptr;
    lv_obj_t* music_time_label_ = nullptr;
    lv_obj_t* music_pause_label_ = nullptr;
    int music_position_ = 0;
    int music_duration_ = 0;
    bool music_paused_ = false;
    bool music_visible_ = false;
    // 歌手行右侧拼上播放模式（"儿歌多多 · 单曲循环"），省一块地方
    std::string music_artist_text_;
    std::string music_mode_text_;
    int music_tick_ = 0;
    esp_timer_handle_t music_timer_ = nullptr;
    void MusicTick();
    void UpdateMusicLabels();

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

#endif
