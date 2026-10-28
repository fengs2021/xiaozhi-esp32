#pragma once

#include <cstdint>
#include <functional>

#include <driver/gpio.h>
#include <esp_timer.h>


// ---------- 夜间模式 ----------
// 到点之后屏幕亮度上限压到 kNightModeMaxBrightness，免得孩子半夜被屏幕亮醒；
// 白天（以及 0 = 关屏）不受影响。时间取系统时间：它由 OTA 下发的 server_time
// 写入，已经按服务端时区偏移过，所以这里用 gmtime 拿到的就是本地时间。
// 想改时间段只改下面两个常量（单位：分钟，21:30 = 21 * 60 + 30）。
constexpr int kNightModeStartMinute = 22 * 60;   // 22:00 起进入夜间模式
constexpr int kNightModeEndMinute = 7 * 60;      // 次日 07:00 退出
constexpr uint8_t kNightModeMaxBrightness = 1;   // 夜间最高只到 1%

class Backlight {
public:
    Backlight();
    ~Backlight();

    void RestoreBrightness();
    void SetBrightness(uint8_t brightness, bool permanent = false);
    inline uint8_t brightness() const { return brightness_; }
    // 现在是不是夜间模式时段（系统时间没同步时一律返回 false）
    static bool IsNightTime();

protected:
    void OnTransitionTimer();
    void OnNightTick();
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;

    esp_timer_handle_t transition_timer_ = nullptr;
    esp_timer_handle_t night_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t step_ = 1;
    bool night_active_ = false;
    // 夜间被压掉的亮度，天亮后按它恢复
    uint8_t night_restore_brightness_ = 0;
};


class PwmBacklight : public Backlight {
public:
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;
};
