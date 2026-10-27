#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"
#include "text_glyph.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class LvglFont;
class LvglImage;

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
    virtual std::shared_ptr<LvglFont> GetTextFont() const { return nullptr; }

private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    // Returns true when the requested emotion was accepted and set in the rendering
    // system. False means invalid input, a missing resource, an uninitialised display, or
    // an unsupported operation. For asynchronous displays, true does not promise that the
    // physical refresh has already completed. Callers are free to ignore the result.
    virtual bool SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    // Remove a matching message without clearing newer content, when supported.
    virtual void RemoveChatMessage(const char* role, const char* content) {}
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) { return false; }
    virtual void ClearTextGlyphs() {}
    virtual void SetEmojiCollection(std::shared_ptr<EmojiCollection>) {}
    virtual void SetupUI() { setup_ui_called_ = true; }
    virtual bool IsMonochrome() const { return false; }
    virtual bool SupportsGuiOperations() const { return false; }
    // 倒计时叠加层：只盖屏幕，独立于表情和聊天气泡。音频链路（唤醒词、VAD、
    // 对话）完全不受影响 —— 倒计时期间照样能叫醒、能插话，秒数继续走。
    virtual void ShowCountdown(int seconds, const char* text) {
        (void)seconds;
        (void)text;
    }
    virtual void HideCountdown() {}
    virtual bool IsCountdownVisible() const { return false; }
    // 音乐播放叠加层：歌名、歌手、进度条、暂停态。纯展示，本板无触摸，
    // 切歌暂停还是靠语音和网页控制；期间不改音频链路。
    // mode 是给人看的模式名（"单曲循环" 等），可为 nullptr
    virtual void ShowMusic(const char* title, const char* artist, int duration_seconds,
                           int position_seconds, bool paused, const char* mode = nullptr) {
        (void)title;
        (void)artist;
        (void)duration_seconds;
        (void)position_seconds;
        (void)paused;
        (void)mode;
    }
    virtual void UpdateMusic(int position_seconds, int duration_seconds, bool paused) {
        (void)position_seconds;
        (void)duration_seconds;
        (void)paused;
    }
    virtual void HideMusic() {}
    virtual bool IsMusicVisible() const { return false; }
    virtual void SetHideSubtitle(bool hide) { (void)hide; }
    virtual bool InsertAnimDialog(const char* name, uint32_t duration_ms) {
        (void)name;
        (void)duration_ms;
        return false;
    }
    virtual bool MountAssets(const char* partition_label) {
        (void)partition_label;
        return false;
    }
    virtual void UnmountAssets() {}
    virtual bool GetAssetData(const std::string& name, const uint8_t*& data, size_t& size) {
        (void)name;
        (void)data;
        (void)size;
        return false;
    }
    virtual void LoadAssets() {}
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    virtual bool SetTextFont(std::shared_ptr<LvglFont> text_font) {
        (void)text_font;
        return false;
    }
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) {
        (void)jpeg_data;
        (void)quality;
        return false;
    }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* display)
        : display_(display), locked_(display_->Lock(30000)) {
        if (!locked_) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        if (locked_) {
            display_->Unlock();
        }
    }

    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;

    explicit operator bool() const { return locked_; }

private:
    Display* display_;
    bool locked_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}
};

#endif
