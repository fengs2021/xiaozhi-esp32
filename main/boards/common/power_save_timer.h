#pragma once

#include <functional>

#include <esp_timer.h>
#include <esp_pm.h>

class PowerSaveTimer {
public:
    // seconds_to_sleep_audio：进入第二级省电（关唤醒词 + 关麦克风输入 + CPU light sleep）
    // 所需的空闲秒数。-1 表示跟随 seconds_to_sleep，保持旧板子的行为不变。
    // 拆成两级是因为「关屏幕」可以很快发生（省电又不打断使用），
    // 而「关唤醒词」晚得多（关了就只能按键唤醒，不能太早）。
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1,
                   int seconds_to_sleep_audio = -1);
    ~PowerSaveTimer();

    void SetEnabled(bool enabled);
    void OnEnterSleepMode(std::function<void()> callback);
    void OnExitSleepMode(std::function<void()> callback);
    // 第二级省电（关唤醒词 + 关麦克风 + light sleep）的回调，用来把屏幕彻底关掉。
    void OnEnterAudioSleepMode(std::function<void()> callback);
    void OnExitAudioSleepMode(std::function<void()> callback);
    void OnShutdownRequest(std::function<void()> callback);
    void WakeUp();

private:
    void PowerSaveCheck();

    esp_timer_handle_t power_save_timer_ = nullptr;
    bool enabled_ = false;
    bool in_sleep_mode_ = false;
    bool in_audio_sleep_ = false;
    bool is_wake_word_running_ = false;
    int ticks_ = 0;
    int cpu_max_freq_;
    int seconds_to_sleep_;
    int seconds_to_shutdown_;
    int seconds_to_sleep_audio_;

    std::function<void()> on_enter_sleep_mode_;
    std::function<void()> on_exit_sleep_mode_;
    std::function<void()> on_enter_audio_sleep_mode_;
    std::function<void()> on_exit_audio_sleep_mode_;
    std::function<void()> on_shutdown_request_;
};
