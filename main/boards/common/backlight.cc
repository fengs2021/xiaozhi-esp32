#include "backlight.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/ledc.h>
#include <ctime>

#define TAG "Backlight"


Backlight::Backlight() {
    // 创建背光渐变定时器
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<Backlight*>(arg);
            self->OnTransitionTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &transition_timer_));

    // 夜间模式检查：每分钟看一眼时间，跨进/跨出夜间时段就立刻调亮度。
    // 这里只碰 LEDC 亮度，不碰 LVGL，所以可以直接在 esp_timer 任务里跑。
    const esp_timer_create_args_t night_args = {
        .callback = [](void* arg) {
            auto self = static_cast<Backlight*>(arg);
            self->OnNightTick();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_night",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&night_args, &night_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(night_timer_, 60 * 1000 * 1000));
    // 开机时先把状态对齐，这样夜里上电也不会先亮一下再变暗
    night_active_ = IsNightTime();
    if (night_active_) {
        ESP_LOGI(TAG, "开机即处于夜间模式，亮度上限 %d%%", kNightModeMaxBrightness);
    }
}

Backlight::~Backlight() {
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
        esp_timer_delete(transition_timer_);
    }
    if (night_timer_ != nullptr) {
        esp_timer_stop(night_timer_);
        esp_timer_delete(night_timer_);
    }
}

void Backlight::RestoreBrightness() {
    // Load brightness from settings
    Settings settings("display");  
    int saved_brightness = settings.GetInt("brightness", 75);
    
    // 检查亮度值是否为0或过小，设置默认值
    if (saved_brightness <= 0) {
        ESP_LOGW(TAG, "Brightness value (%d) is too small, setting to default (10)", saved_brightness);
        saved_brightness = 10;  // 设置一个较低的默认值
    }
    
    SetBrightness(saved_brightness);
}

bool Backlight::IsNightTime() {
    time_t now = time(nullptr);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    // 时间还没同步（OTA 的 server_time 没到）就别瞎判，当成白天
    if (tm_now.tm_year + 1900 < 2025) {
        return false;
    }
    int cur = tm_now.tm_hour * 60 + tm_now.tm_min;
    if (kNightModeStartMinute == kNightModeEndMinute) {
        return false;
    }
    if (kNightModeStartMinute < kNightModeEndMinute) {
        return cur >= kNightModeStartMinute && cur < kNightModeEndMinute;
    }
    // 跨零点，比如 22:00 -> 07:00
    return cur >= kNightModeStartMinute || cur < kNightModeEndMinute;
}

void Backlight::OnNightTick() {
    bool active = IsNightTime();
    if (active == night_active_) {
        return;
    }
    night_active_ = active;
    if (active) {
        // 记下现在本来该多亮，天亮后恢复；已经关屏（0）就不用管
        if (target_brightness_ > kNightModeMaxBrightness) {
            night_restore_brightness_ = target_brightness_;
        }
        ESP_LOGI(TAG, "进入夜间模式，亮度压到 %d%%（原本 %d%%）",
                 kNightModeMaxBrightness, target_brightness_);
        if (brightness_ > kNightModeMaxBrightness) {
            SetBrightness(kNightModeMaxBrightness);
        }
    } else {
        uint8_t restore = night_restore_brightness_ > kNightModeMaxBrightness
                              ? night_restore_brightness_
                              : 75;
        ESP_LOGI(TAG, "退出夜间模式，恢复亮度 %d%%", restore);
        if (brightness_ > 0) {
            SetBrightness(restore);
        }
    }
}

void Backlight::SetBrightness(uint8_t brightness, bool permanent) {
    if (brightness > 100) {
        brightness = 100;
    }

    // 夜间模式：无论谁来要亮度（省电定时器唤醒、RestoreBrightness、服务端 MCP 下发），
    // 一律压到上限。0（关屏）保持 0，不会被"抬亮"。
    uint8_t wanted = brightness;
    if (night_active_ && brightness > kNightModeMaxBrightness) {
        night_restore_brightness_ = brightness;
        brightness = kNightModeMaxBrightness;
    }

    if (brightness_ == brightness) {
        return;
    }

    if (permanent) {
        Settings settings("display", true);
        // 存调用方真正想要的那个值，不然夜里把亮度调到 1% 会被写进 NVS，
        // 白天开机也只剩 1%
        settings.SetInt("brightness", wanted);
    }

    target_brightness_ = brightness;
    step_ = (target_brightness_ > brightness_) ? 1 : -1;

    if (transition_timer_ != nullptr) {
        // 启动定时器，每 5ms 更新一次
        esp_timer_start_periodic(transition_timer_, 5 * 1000);
    }
    ESP_LOGI(TAG, "Set brightness to %d", brightness);
}

void Backlight::OnTransitionTimer() {
    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
        return;
    }

    brightness_ += step_;
    SetBrightnessImpl(brightness_);

    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
    }
}

PwmBacklight::PwmBacklight(gpio_num_t pin, bool output_invert, uint32_t freq_hz) : Backlight() {
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq_hz, //背光pwm频率需要高一点，防止电感啸叫
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));

    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = output_invert,
        }
    };
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

PwmBacklight::~PwmBacklight() {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

void PwmBacklight::SetBrightnessImpl(uint8_t brightness) {
    // LEDC resolution set to 10bits, thus: 100% = 1023
    uint32_t duty_cycle = (1023 * brightness) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

