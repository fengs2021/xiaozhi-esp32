#include "power_save_timer.h"
#include "application.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_log.h>

#define TAG "PowerSaveTimer"


PowerSaveTimer::PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep, int seconds_to_shutdown,
                               int seconds_to_sleep_audio)
    : cpu_max_freq_(cpu_max_freq),
      seconds_to_sleep_(seconds_to_sleep),
      seconds_to_shutdown_(seconds_to_shutdown),
      seconds_to_sleep_audio_(seconds_to_sleep_audio) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<PowerSaveTimer*>(arg);
            self->PowerSaveCheck();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_save_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_save_timer_));
}

PowerSaveTimer::~PowerSaveTimer() {
    esp_timer_stop(power_save_timer_);
    esp_timer_delete(power_save_timer_);
}

void PowerSaveTimer::SetEnabled(bool enabled) {
    if (enabled && !enabled_) {
        Settings settings("wifi", false);
        if (!settings.GetBool("sleep_mode", true)) {
            ESP_LOGI(TAG, "Power save timer is disabled by settings");
            return;
        }

        ticks_ = 0;
        enabled_ = enabled;
        ESP_ERROR_CHECK(esp_timer_start_periodic(power_save_timer_, 1000000));
        ESP_LOGI(TAG, "Power save timer enabled");
    } else if (!enabled && enabled_) {
        ESP_ERROR_CHECK(esp_timer_stop(power_save_timer_));
        enabled_ = enabled;
        WakeUp();
        ESP_LOGI(TAG, "Power save timer disabled");
    }
}

void PowerSaveTimer::OnEnterSleepMode(std::function<void()> callback) {
    on_enter_sleep_mode_ = callback;
}

void PowerSaveTimer::OnExitSleepMode(std::function<void()> callback) {
    on_exit_sleep_mode_ = callback;
}

void PowerSaveTimer::OnEnterAudioSleepMode(std::function<void()> callback) {
    on_enter_audio_sleep_mode_ = callback;
}

void PowerSaveTimer::OnExitAudioSleepMode(std::function<void()> callback) {
    on_exit_audio_sleep_mode_ = callback;
}

void PowerSaveTimer::OnShutdownRequest(std::function<void()> callback) {
    on_shutdown_request_ = callback;
}

void PowerSaveTimer::PowerSaveCheck() {
    auto& app = Application::GetInstance();
    bool busy = app.GetDeviceState() != kDeviceStateIdle;

    // 设备一忙（开始聆听 / 正在说话 / 播放回复 / 连网），任何一级省电都要立刻退出并把
    // 计时清零。原厂只对第二级做了这个判断，第一级进了之后哪怕孩子在说话，屏幕也一直
    // 停在 1% 背光，而且计数继续往上累加，最终会在对话中途跨过第二级阈值——把麦克风
    // 和唤醒词关掉，表现为「说着话突然就断线了」。
    if (busy) {
        if (in_sleep_mode_ || in_audio_sleep_) {
            ESP_LOGI(TAG, "Device is busy, exiting power save mode");
            WakeUp();
        } else {
            ticks_ = 0;
        }
        return;
    }

    // 状态是 idle 但音频还没停下来（最后一段 TTS 还在播）也不算空闲。
    if (!app.CanEnterSleepMode()) {
        ticks_ = 0;
        return;
    }

    ticks_++;

    // 第一级：关屏幕 / 降背光。由板子的 OnEnterSleepMode 回调决定具体动作。
    if (seconds_to_sleep_ != -1 && ticks_ >= seconds_to_sleep_ && !in_sleep_mode_) {
        ESP_LOGI(TAG, "Enabling power save mode");
        in_sleep_mode_ = true;
        // 回调里会操作 LVGL（SetPowerSaveMode → SetChatMessage / SetEmotion），而本函数是
        // esp_timer 回调，跑在 esp_timer 任务里。LVGL 不是线程安全的，跨任务直接改会跟
        // 主任务的渲染撞车：轻则花屏，重则主任务卡死——主任务卡住之后 WebSocket 不再处理
        // 消息，服务端心跳 30 秒超时就把连接关掉，设备就「离线」了。
        app.Schedule([this]() {
            if (on_enter_sleep_mode_) {
                on_enter_sleep_mode_();
            }
        });
    }

    // 第二级：关唤醒词 + 关麦克风输入 + CPU light sleep（WiFi 保持连接）。
    // seconds_to_sleep_audio_ 为 -1 时跟随第一级阈值，这样只传三个参数的旧板子行为完全不变。
    int audio_threshold = (seconds_to_sleep_audio_ != -1) ? seconds_to_sleep_audio_ : seconds_to_sleep_;
    if (cpu_max_freq_ != -1 && audio_threshold != -1 && ticks_ >= audio_threshold && !in_audio_sleep_) {
        ESP_LOGI(TAG, "Enabling audio power save mode");
        in_audio_sleep_ = true;

        // 关唤醒词、关麦克风、关屏这三件事都要动 AudioService 和 LVGL，同样不能在
        // esp_timer 任务里直接干，统一丢回主任务。esp_pm_configure 是线程安全的，留在原地。
        app.Schedule([this]() {
            auto& app_ref = Application::GetInstance();
            auto& audio_service = app_ref.GetAudioService();
            is_wake_word_running_ = audio_service.IsWakeWordRunning();
            if (is_wake_word_running_) {
                audio_service.EnableWakeWordDetection(false);
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec) {
                codec->EnableInput(false);
            }

            // 交给板子把屏幕彻底关掉（背光归零 + 关面板）。1% 背光肉眼看不见但仍在耗电，
            // 长期停在静止画面上也有残影风险。
            if (on_enter_audio_sleep_mode_) {
                on_enter_audio_sleep_mode_();
            }
        });

        esp_pm_config_t pm_config = {
            .max_freq_mhz = cpu_max_freq_,
            .min_freq_mhz = 40,
            .light_sleep_enable = true,
        };
        esp_err_t err = esp_pm_configure(&pm_config);
        if (err != ESP_OK) {
            // 常见原因：没开 CONFIG_PM_ENABLE 或 CONFIG_FREERTOS_USE_TICKLESS_IDLE。
            // 这时 light sleep 不生效（功耗降不下来），但屏幕、麦克风、唤醒词已经关了，
            // 设备只能按键唤醒——这是预期行为，不能因此把屏幕又亮起来。
            ESP_LOGW(TAG, "esp_pm_configure failed (%s), CPU light sleep skipped", esp_err_to_name(err));
        }
    }

    if (seconds_to_shutdown_ != -1 && ticks_ >= seconds_to_shutdown_ && on_shutdown_request_) {
        on_shutdown_request_();
    }
}

void PowerSaveTimer::WakeUp() {
    ticks_ = 0;

    if (in_audio_sleep_) {
        ESP_LOGI(TAG, "Exiting audio power save mode");
        in_audio_sleep_ = false;

        esp_pm_config_t pm_config = {
            .max_freq_mhz = cpu_max_freq_,
            .min_freq_mhz = cpu_max_freq_,
            .light_sleep_enable = false,
        };
        esp_pm_configure(&pm_config);

        // 恢复唤醒词和亮屏同样要回主任务，理由见 PowerSaveCheck。
        auto& app = Application::GetInstance();
        app.Schedule([this]() {
            auto& app_ref = Application::GetInstance();
            auto& audio_service = app_ref.GetAudioService();
            // 麦克风输入不用手动开回来，AudioService::ReadAudioData 发现输入关了会自己打开。
            if (is_wake_word_running_) {
                audio_service.EnableWakeWordDetection(true);
            }
            if (on_exit_audio_sleep_mode_) {
                on_exit_audio_sleep_mode_();
            }
        });
    }

    if (in_sleep_mode_) {
        ESP_LOGI(TAG, "Exiting power save mode");
        in_sleep_mode_ = false;
        auto& app = Application::GetInstance();
        app.Schedule([this]() {
            if (on_exit_sleep_mode_) {
                on_exit_sleep_mode_();
            }
        });
    }
}
