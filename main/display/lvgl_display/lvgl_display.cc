#include <esp_err.h>
#include <esp_log.h>
#include <material_symbols.h>
#include <cstdlib>
#include <cstring>
#include <string>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "dynamic_glyph_cache.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "Display"

LvglDisplay::LvglDisplay() {
    dynamic_glyph_cache_ = std::make_unique<DynamicGlyphCache>();
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback =
            [](void* arg) {
                LvglDisplay* display = static_cast<LvglDisplay*>(arg);
                DisplayLockGuard lock(display);
                lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

bool LvglDisplay::SetTextFont(std::shared_ptr<LvglFont> text_font) {
    if (text_font == nullptr || text_font->font() == nullptr) {
        return false;
    }

    DisplayLockGuard lock(this);
    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");
    if (light_theme == nullptr && dark_theme == nullptr) {
        return false;
    }

    // LVGL styles keep raw lv_font_t pointers. Keep the previous font owners alive until the
    // current theme has rebound every style to the new font.
    auto previous_light_font = light_theme != nullptr ? light_theme->text_font() : nullptr;
    auto previous_dark_font = dark_theme != nullptr ? dark_theme->text_font() : nullptr;
    if (light_theme != nullptr) {
        light_theme->set_text_font(text_font);
    }
    if (dark_theme != nullptr) {
        dark_theme->set_text_font(text_font);
    }
    if (current_theme_ != nullptr) {
        SetTheme(current_theme_);
    }
    previous_light_font.reset();
    previous_dark_font.reset();
    return true;
}

bool LvglDisplay::AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) {
    if (dynamic_glyph_cache_ == nullptr) {
        return false;
    }
    if (glyphs.empty()) {
        if (!TextGlyphStorageUsesPsram()) {
            ClearTextGlyphs();
        }
        return false;
    }
    if (bpp != 1 && bpp != 4) {
        return false;
    }

    DisplayLockGuard lock(this);
    if (!lock || current_theme_ == nullptr) {
        return false;
    }
    auto text_font = current_theme_->GetTextFont();
    if (text_font == nullptr) {
        return false;
    }

    auto fallback = dynamic_glyph_cache_->EnsureFont(text_font->font(), bpp);
    if (fallback == nullptr) {
        return false;
    }
    text_font->SetFallback(fallback);
    return dynamic_glyph_cache_->AddGlyphs(glyphs);
}

void LvglDisplay::ClearTextGlyphs() {
    if (dynamic_glyph_cache_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    dynamic_glyph_cache_->Clear();
}

LvglDisplay::~LvglDisplay() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if (low_battery_popup_ != nullptr) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but "
                     "label not created)",
                     status);
        }
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!",
                 notification);
    }
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "ShowNotification('%s') failed: notification_label_ is nullptr (SetupUI() was "
                     "called but label not created)",
                     notification);
        }
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // Update icon if mute state changes
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, MATERIAL_SYMBOLS_VOLUME_OFF);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time — show HH:MM when idle; refresh immediately on state
    // transitions (update_all=true) and on every minute change thereafter.
    if (app.GetDeviceState() == kDeviceStateIdle) {
        time_t now = time(NULL);
        struct tm* tm_now = localtime(&now);
        if (tm_now->tm_year >= 2025 - 1900) {
            int cur_min = tm_now->tm_hour * 60 + tm_now->tm_min;
            if (update_all || cur_min != last_displayed_clock_min_) {
                last_displayed_clock_min_ = cur_min;
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", tm_now);
                SetStatus(time_str);
            }
        } else {
            ESP_LOGW(TAG, "System time not set (tm_year=%d)", tm_now->tm_year);
        }
    } else {
        // Reset so the clock re-appears immediately when idle resumes.
        last_displayed_clock_min_ = -1;
    }

    esp_pm_lock_acquire(pm_lock_);
    // Update battery icon
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
        } else {
            const char* levels[] = {
                MATERIAL_SYMBOLS_BATTERY_ANDROID_0,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6,
                MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
            };
            int level_index = battery_level <= 0
                                  ? 0
                                  : (battery_level >= 100 ? 7 : 1 + ((battery_level - 1) * 6 / 99));
            icon = levels[level_index];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        // Check low battery popup only when clock tick event is triggered
        // Because when initializing, the battery level is not ready yet.
        if (low_battery_popup_ != nullptr && !update_all) {
            if (strcmp(icon, MATERIAL_SYMBOLS_BATTERY_ANDROID_0) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_,
                                    LV_OBJ_FLAG_HIDDEN)) {  // Show if low battery popup is hidden
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.Schedule([&app]() { app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY); });
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_,
                                     LV_OBJ_FLAG_HIDDEN)) {  // Hide if low battery popup is shown
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // Update network icon every 10 seconds
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,      kDeviceStateStarting,   kDeviceStateWifiConfiguring,
            kDeviceStateListening, kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) !=
            allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {}

// ---------- 倒计时叠加层 ----------
// 全程只碰屏幕对象：不改音频状态、不动 VAD、不开关注唤醒词，所以倒计时期间
// 设备照样能被叫醒、能正常对话，秒数在独立定时器里继续走。
void LvglDisplay::UpdateCountdownLabel() {
    if (countdown_num_label_ == nullptr) {
        return;
    }
    int total = countdown_remaining_ > 0 ? countdown_remaining_ : 0;
    int hours = total / 3600;
    int minutes = (total % 3600) / 60;
    int seconds = total % 60;
    char buf[16];
    if (hours > 0) {
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, minutes, seconds);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
    }
    lv_label_set_text(countdown_num_label_, buf);
}

void LvglDisplay::ShowCountdown(int seconds, const char* text) {
    if (seconds <= 0) {
        HideCountdown();
        return;
    }
    DisplayLockGuard lock(this);
    countdown_remaining_ = seconds;

    if (countdown_container_ == nullptr) {
        lv_obj_t* screen = lv_screen_active();
        countdown_container_ = lv_obj_create(screen);
        lv_obj_remove_style_all(countdown_container_);
        lv_obj_set_size(countdown_container_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(countdown_container_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(countdown_container_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(countdown_container_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(countdown_container_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(countdown_container_, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        countdown_num_label_ = lv_label_create(countdown_container_);
        lv_obj_set_style_text_color(countdown_num_label_, lv_color_white(), 0);
        lv_obj_set_style_text_font(countdown_num_label_, &lv_font_montserrat_48, 0);

        countdown_hint_label_ = lv_label_create(countdown_container_);
        lv_obj_set_style_text_color(countdown_hint_label_, lv_color_hex(0x9A9A9A), 0);
        lv_obj_set_style_text_font(countdown_hint_label_, &lv_font_montserrat_14, 0);
    }

    // 中文提示要服务端按需下发字形，倒计时的关键信息是秒数本身，这里只记日志。
    if (text != nullptr && text[0] != '\0') {
        ESP_LOGI(TAG, "Countdown for: %s", text);
    }
    lv_obj_move_foreground(countdown_container_);
    lv_obj_remove_flag(countdown_container_, LV_OBJ_FLAG_HIDDEN);
    countdown_visible_ = true;
    UpdateCountdownLabel();

    if (countdown_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = [](void* arg) {
            static_cast<LvglDisplay*>(arg)->CountdownTick();
        };
        timer_args.arg = this;
        timer_args.name = "countdown";
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &countdown_timer_));
    }
    esp_timer_stop(countdown_timer_);
    esp_timer_start_periodic(countdown_timer_, 1000000);
    ESP_LOGI(TAG, "Countdown started: %d s", seconds);
}

void LvglDisplay::HideCountdown() {
    if (countdown_timer_ != nullptr) {
        esp_timer_stop(countdown_timer_);
    }
    countdown_remaining_ = 0;
    countdown_visible_ = false;
    if (countdown_container_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_obj_add_flag(countdown_container_, LV_OBJ_FLAG_HIDDEN);
    // 倒计时收起来以后，正在放歌的界面要重新回到最上层
    if (music_visible_ && music_container_ != nullptr) {
        lv_obj_move_foreground(music_container_);
    }
}

void LvglDisplay::CountdownTick() {
    if (countdown_remaining_ <= 0) {
        return;
    }
    countdown_remaining_--;
    bool finished = countdown_remaining_ <= 0;
    {
        DisplayLockGuard lock(this);
        if (countdown_container_ != nullptr) {
            if (finished) {
                lv_obj_add_flag(countdown_container_, LV_OBJ_FLAG_HIDDEN);
            } else {
                UpdateCountdownLabel();
            }
        }
        if (finished) {
            countdown_visible_ = false;
        }
    }
    // 到点由服务端播报并下发 countdown_hide；这里只是兜底，别让屏上停住。
    if (finished && countdown_timer_ != nullptr) {
        esp_timer_stop(countdown_timer_);
    }
}

// ---------- 音乐播放叠加层 ----------
// 纯展示：这块 1.54 寸屏没有触摸，切歌/暂停仍然靠语音和网页控制。
// 全程只碰屏幕对象，不改音频状态，唤醒词和对话链路照常工作。
namespace {
// 5 根柱子的跳动表：每 250ms 换一行，看起来像在打拍子（不引随机数，省 RAM）
constexpr uint8_t kMusicBarPattern[8][5] = {
    {26, 12, 20, 9, 23},  {15, 24, 10, 21, 12}, {9, 19, 26, 14, 17},
    {21, 10, 15, 26, 9},  {12, 22, 9, 17, 24},  {24, 9, 21, 10, 15},
    {10, 17, 13, 24, 20}, {19, 13, 24, 15, 10},
};
}  // namespace

void LvglDisplay::UpdateMusicLabels() {
    if (music_time_label_ == nullptr) {
        return;
    }
    int pos = music_position_ > 0 ? music_position_ : 0;
    int dur = music_duration_ > 0 ? music_duration_ : 0;
    if (dur > 0 && pos > dur) {
        pos = dur;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d / %d:%02d", pos / 60, pos % 60, dur / 60, dur % 60);
    lv_label_set_text(music_time_label_, buf);
    if (music_artist_label_ != nullptr) {
        std::string line = music_artist_text_;
        if (!music_mode_text_.empty()) {
            line += line.empty() ? music_mode_text_ : " · " + music_mode_text_;
        }
        lv_label_set_text(music_artist_label_, line.c_str());
    }
    if (music_progress_ != nullptr && dur > 0) {
        lv_bar_set_range(music_progress_, 0, dur);
        lv_bar_set_value(music_progress_, pos, LV_ANIM_OFF);
    }
    if (music_pause_label_ != nullptr) {
        if (music_paused_) {
            lv_obj_remove_flag(music_pause_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(music_pause_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LvglDisplay::ShowMusic(const char* title, const char* artist, int duration_seconds,
                            int position_seconds, bool paused, const char* mode) {
    DisplayLockGuard lock(this);
    music_duration_ = duration_seconds > 0 ? duration_seconds : 0;
    music_position_ = position_seconds > 0 ? position_seconds : 0;
    music_paused_ = paused;
    music_artist_text_ = artist != nullptr ? artist : "";
    // mode 为空时保留上一次的模式名（切歌不会带空串，这里只是防御）
    if (mode != nullptr) {
        music_mode_text_ = mode;
    }

    if (music_container_ == nullptr) {
        // 歌名和歌手是中文，得用主题里的 CBin 字体（聊天字幕用的就是它）
        const lv_font_t* text_font = &lv_font_montserrat_14;
        if (current_theme_ != nullptr) {
            auto theme_font = current_theme_->GetTextFont();
            if (theme_font != nullptr && theme_font->font() != nullptr) {
                text_font = theme_font->font();
            }
        }

        lv_obj_t* screen = lv_screen_active();
        music_container_ = lv_obj_create(screen);
        lv_obj_remove_style_all(music_container_);
        lv_obj_set_size(music_container_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(music_container_, lv_color_hex(0x101519), 0);
        lv_obj_set_style_bg_opa(music_container_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(music_container_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(music_container_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(music_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(music_container_, 8, 0);

        music_bars_row_ = lv_obj_create(music_container_);
        lv_obj_remove_style_all(music_bars_row_);
        lv_obj_set_size(music_bars_row_, 120, 32);
        lv_obj_remove_flag(music_bars_row_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(music_bars_row_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(music_bars_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(music_bars_row_, 6, 0);
        for (int i = 0; i < 5; ++i) {
            music_bars_[i] = lv_obj_create(music_bars_row_);
            lv_obj_remove_style_all(music_bars_[i]);
            lv_obj_set_width(music_bars_[i], 8);
            lv_obj_set_height(music_bars_[i], kMusicBarPattern[0][i]);
            lv_obj_set_style_bg_color(music_bars_[i], lv_color_hex(0x4DD0E1), 0);
            lv_obj_set_style_bg_opa(music_bars_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_radius(music_bars_[i], 4, 0);
        }

        music_title_label_ = lv_label_create(music_container_);
        lv_obj_set_width(music_title_label_, 200);
        lv_obj_set_style_text_align(music_title_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(music_title_label_, lv_color_white(), 0);
        lv_obj_set_style_text_font(music_title_label_, text_font, 0);
        // 长歌名不打折行：走马灯滚一圈，屏太小，折行会把进度条挤出屏
        lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

        music_artist_label_ = lv_label_create(music_container_);
        lv_obj_set_width(music_artist_label_, 200);
        lv_obj_set_style_text_align(music_artist_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(music_artist_label_, lv_color_hex(0x9AA4AE), 0);
        lv_obj_set_style_text_font(music_artist_label_, text_font, 0);
        lv_label_set_long_mode(music_artist_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);

        music_progress_ = lv_bar_create(music_container_);
        lv_obj_set_size(music_progress_, 200, 6);
        lv_bar_set_range(music_progress_, 0, 100);
        lv_bar_set_value(music_progress_, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(music_progress_, lv_color_hex(0x2A3138), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(music_progress_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(music_progress_, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(music_progress_, lv_color_hex(0x4DD0E1), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(music_progress_, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(music_progress_, 3, LV_PART_INDICATOR);

        music_time_label_ = lv_label_create(music_container_);
        lv_obj_set_style_text_color(music_time_label_, lv_color_hex(0x9AA4AE), 0);
        lv_obj_set_style_text_font(music_time_label_, &lv_font_montserrat_14, 0);
        lv_label_set_text(music_time_label_, "0:00 / 0:00");

        music_pause_label_ = lv_label_create(music_container_);
        lv_obj_set_style_text_color(music_pause_label_, lv_color_hex(0xFFC857), 0);
        lv_obj_set_style_text_font(music_pause_label_, text_font, 0);
        lv_label_set_text(music_pause_label_, "暂停");
        lv_obj_add_flag(music_pause_label_, LV_OBJ_FLAG_HIDDEN);
    }

    if (title != nullptr) {
        lv_label_set_text(music_title_label_, title);
    }
    UpdateMusicLabels();

    lv_obj_remove_flag(music_container_, LV_OBJ_FLAG_HIDDEN);
    music_visible_ = true;
    // 倒计时优先：它盖在音乐上面，屏幕只有 240x240，倒计时是更急的信息
    if (countdown_visible_ && countdown_container_ != nullptr) {
        lv_obj_move_foreground(countdown_container_);
    } else {
        lv_obj_move_foreground(music_container_);
    }

    if (music_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = [](void* arg) {
            static_cast<LvglDisplay*>(arg)->MusicTick();
        };
        timer_args.arg = this;
        timer_args.name = "music_ui";
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &music_timer_));
    }
    esp_timer_stop(music_timer_);
    music_tick_ = 0;
    esp_timer_start_periodic(music_timer_, 250000);
    ESP_LOGI(TAG, "Music UI: %s (dur=%d, pos=%d, paused=%d, mode=%s)",
             title != nullptr ? title : "", music_duration_, music_position_, paused ? 1 : 0,
             music_mode_text_.c_str());
}

void LvglDisplay::UpdateMusic(int position_seconds, int duration_seconds, bool paused) {
    DisplayLockGuard lock(this);
    music_position_ = position_seconds > 0 ? position_seconds : 0;
    if (duration_seconds > 0) {
        music_duration_ = duration_seconds;
    }
    music_paused_ = paused;
    UpdateMusicLabels();
}

void LvglDisplay::HideMusic() {
    if (music_timer_ != nullptr) {
        esp_timer_stop(music_timer_);
    }
    music_visible_ = false;
    music_paused_ = false;
    music_tick_ = 0;
    if (music_container_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_obj_add_flag(music_container_, LV_OBJ_FLAG_HIDDEN);
}

void LvglDisplay::MusicTick() {
    if (!music_visible_) {
        return;
    }
    music_tick_++;
    DisplayLockGuard lock(this);
    if (!music_paused_) {
        for (int i = 0; i < 5; ++i) {
            if (music_bars_[i] != nullptr) {
                lv_obj_set_height(music_bars_[i], kMusicBarPattern[music_tick_ & 7][i]);
            }
        }
    }
    // 每秒推进一格。服务端会在开始/切歌/暂停/恢复时下发准确位置校准，
    // 这里只是让屏幕别卡住不动。
    if ((music_tick_ & 3) == 0 && !music_paused_ && music_duration_ > 0) {
        if (music_position_ < music_duration_) {
            music_position_++;
        }
        UpdateMusicLabels();
    }
}

void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret =
        image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w,
                         draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
                         [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                             std::string* output = static_cast<std::string*>(arg);
                             if (data && len > 0) {
                                 output->append(static_cast<const char*>(data), len);
                             }
                             return len;
                         },
                         &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}
