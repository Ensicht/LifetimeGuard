 
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include "GuardUiSettings.h"
#include "GuardBuild.h"

namespace dstorage_guard {

struct GuardStatusSnapshot {
    std::uint32_t initialize_state{};
    std::uint32_t file_state{};
    std::uint32_t resource_state{};
    std::uint32_t resource_mode{};
    std::uint32_t iat_state{};
    bool lifecycle_available{};
    bool loading{};
    std::uint64_t active_leases{};
    std::uint64_t deferred_closes{};
    std::uint64_t texture_recoveries{};
    int resource_create_status{};
    int resource_enable_status{};
    std::uint32_t retry_state{};
    std::uint64_t retry_queued{};
    std::uint64_t retry_recovered{};
    std::uint64_t retry_failed{};
    int retry_create_status{};
    int retry_enable_status{};
    bool gameplay_retry{};
    std::uint64_t retry_inflight{};
    std::uint64_t gameplay_queued{};
    std::uint64_t retry_backoffs{};
    std::uint64_t retry_events_dropped{};
    bool loose_cleanup_ready{};
    std::uint64_t loose_cleanup_pairs{};
};

inline const char *guard_health(const GuardStatusSnapshot &s,
                                GuardLanguage language = GuardLanguage::Chinese) noexcept {
    const auto tr = [language](const char *zh, const char *en) {
        return localized(language, zh, en);
    };
    if (s.initialize_state < 2) {
        return tr("正在初始化", "Initializing");
    }
    if (!s.lifecycle_available) {
        return tr("保护未启用：加载生命周期不可用",
                  "Protection unavailable: loading lifecycle missing");
    }
    if (s.file_state == 1 && s.resource_state == 1 && s.resource_mode != 0) {
        return s.retry_state == 2 ? tr("文件、纹理收尾与重读保护均已就绪",
                                       "File, texture completion and reread protections ready")
                                  : tr("原有两层保护已就绪；纹理重读尚未就绪",
                                       "Original protections ready; texture reread not ready");
    }
    if (s.file_state == 1) {
        return s.resource_state == 2
                   ? tr("文件保护就绪，纹理保护安装中",
                        "File protection ready; installing texture protection")
                   : tr("保护不完整：仅文件保护，纹理保护未安装",
                        "Partial protection: file only, texture protection missing");
    }
    if (s.file_state == 2) {
        return tr("正在安装文件保护", "Installing file protection");
    }
    if (s.file_state == 0 && s.iat_state == 1) {
        return tr("等待 DirectStorage 初始化", "Waiting for DirectStorage initialization");
    }
    return tr("保护安装失败", "Protection installation failed");
}

inline const char *guard_layer_state(std::uint32_t state,
                                     GuardLanguage language = GuardLanguage::Chinese) noexcept {
    const auto tr = [language](const char *zh, const char *en) {
        return localized(language, zh, en);
    };
    switch (state) {
    case 0:
        return tr("尚未安装", "Not installed");
    case 1:
        return tr("已安装", "Installed");
    case 2:
        return tr("安装中", "Installing");
    case 3:
        return tr("失败：目标结构不匹配", "Failed: target layout mismatch");
    case 4:
        return tr("失败：Hook 库初始化失败", "Failed: hook library initialization");
    case 5:
        return tr("失败：Hook 创建失败或备用入口验证未通过",
                  "Failed: hook creation or fallback validation");
    case 6:
        return tr("失败：Hook 启用失败", "Failed: hook enable");
    case 7:
        return tr("停用：加载生命周期不可用", "Disabled: loading lifecycle missing");
    default:
        return tr("未知状态", "Unknown state");
    }
}

 
 
class GuardStatusUi {
  public:
    bool load(HMODULE host) noexcept {
        ready_ = false;
        if (host == nullptr) {
            return false;
        }
        context_ = reinterpret_cast<ContextFn>(GetProcAddress(host, "igGetCurrentContext"));
        tree_ = reinterpret_cast<TreeFn>(GetProcAddress(host, "igTreeNode_Str"));
        pop_ = reinterpret_cast<VoidFn>(GetProcAddress(host, "igTreePop"));
        text_ = reinterpret_cast<TextFn>(GetProcAddress(host, "igTextUnformatted"));
        wrap_ = reinterpret_cast<WrapFn>(GetProcAddress(host, "igPushTextWrapPos"));
        unwrap_ = reinterpret_cast<VoidFn>(GetProcAddress(host, "igPopTextWrapPos"));
        separator_ = reinterpret_cast<VoidFn>(GetProcAddress(host, "igSeparator"));
        combo_ = reinterpret_cast<ComboFn>(GetProcAddress(host, "igCombo_Str_arr"));
        width_ = reinterpret_cast<WrapFn>(GetProcAddress(host, "igSetNextItemWidth"));
        ready_ = context_ && tree_ && pop_ && text_ && wrap_ && unwrap_ && separator_;
        return ready_;
    }

    void configure_settings(const wchar_t *path) noexcept {
        settings_.load(path);
    }

    void draw(void *context, const GuardStatusSnapshot &s) noexcept {
        if (!ready_ || context == nullptr || context_() != context) {
            return;
        }
        if (!tree_("LifetimeGuard v" LG_BUILD_TEXT "###DStorageFileLifetimeGuard")) {
            return;
        }
        wrap_(0.0F);
        if (combo_) {
            const char *languages[] = {"中文", "English"};
            int selected = settings_.language() == GuardLanguage::English ? 1 : 0;
            if (width_) {
                width_(180.0F);
            }
            if (combo_("语言 / Language##DStorageGuardLanguage", &selected, languages, 2, -1) &&
                selected >= 0 && selected < 2) {
                settings_.set_language(selected == 1 ? GuardLanguage::English
                                                     : GuardLanguage::Chinese);
            }
        }
        const auto language = settings_.language();
        const auto tr = [language](const char *zh, const char *en) {
            return localized(language, zh, en);
        };
        if (settings_.save_failed()) {
            line(tr("语言设置未保存，下次启动可能恢复默认。",
                    "Language could not be saved; next launch may use the default."));
        }
        line(guard_health(s, language));
        separator_();
        const bool installed =
            s.lifecycle_available && (s.file_state == 1 || s.resource_state == 1);
        line(!installed
                 ? tr("运行状态：保护未就绪", "Status: protection not ready")
                 : (s.loading
                        ? tr("运行状态：加载中，保护已启用", "Status: loading, protection active")
                        : (s.gameplay_retry && s.retry_state == 2
                               ? tr("运行状态：非加载期，保留失败纹理重读与清理配对",
                                    "Status: gameplay, failed-texture rereads and cleanup pairing")
                               : tr("运行状态：非加载期，停止接收新保护请求",
                                    "Status: gameplay, not accepting new protection requests"))));
        label(tr("文件寿命保护", "File lifetime protection"),
              guard_layer_state(s.file_state, language));
        label(tr("纹理恢复保护", "Texture completion protection"),
              guard_layer_state(s.resource_state, language));
        const auto retry_layer =
            s.retry_state == 1 ? 2U : (s.retry_state == 2 ? 1U : s.retry_state);
        label(tr("失败纹理重读", "Failed texture reread"),
              guard_layer_state(retry_layer, language));
        label(tr("加载生命周期", "Loading lifecycle"),
              s.lifecycle_available ? tr("已就绪", "Ready") : tr("不可用", "Unavailable"));
        label(tr("纹理入口", "Texture entry"),
              s.resource_mode == 1
                  ? tr("原生函数 Hook", "Native function hook")
                  : (s.resource_mode == 2 ? tr("任务表备用入口", "Task table fallback")
                                          : tr("未安装", "Not installed")));
        char buffer[512]{};
        label(tr("松散纹理清理配对", "Loose-texture cleanup pairing"),
              s.loose_cleanup_ready
                  ? tr("已就绪（仅原生清理事件）", "Ready (native cleanup events only)")
                  : tr("未启用，请查看故障报告", "Unavailable; see failure report"));
        std::snprintf(buffer, sizeof(buffer),
                      tr("本次清理引用配对：%llu", "Cleanup reference pairs: %llu"),
                      static_cast<unsigned long long>(s.loose_cleanup_pairs));
        line(buffer);
        std::snprintf(buffer, sizeof(buffer),
                      tr("正在排空的文件请求：%llu", "File requests draining: %llu"),
                      static_cast<unsigned long long>(s.active_leases));
        line(buffer);
        std::snprintf(buffer, sizeof(buffer),
                      tr("重读在途：%llu；游玩期提交：%llu；退避：%llu",
                         "Rereads in flight: %llu; gameplay submissions: %llu; backoffs: %llu"),
                      static_cast<unsigned long long>(s.retry_inflight),
                      static_cast<unsigned long long>(s.gameplay_queued),
                      static_cast<unsigned long long>(s.retry_backoffs));
        line(buffer);
        if (s.retry_events_dropped != 0) {
            std::snprintf(
                buffer, sizeof(buffer),
                tr("诊断缓冲已满，省略事件：%llu", "Diagnostic buffer full, omitted events: %llu"),
                static_cast<unsigned long long>(s.retry_events_dropped));
            line(buffer);
        }
        std::snprintf(buffer, sizeof(buffer),
                      tr("纹理重读：已提交 %llu；已恢复 %llu；再次失败 %llu",
                         "Texture rereads: submitted %llu; recovered %llu; failed again %llu"),
                      static_cast<unsigned long long>(s.retry_queued),
                      static_cast<unsigned long long>(s.retry_recovered),
                      static_cast<unsigned long long>(s.retry_failed));
        line(buffer);
        if (s.retry_state >= 3) {
            std::snprintf(buffer, sizeof(buffer),
                          tr("重读安装记录：Create=%d；Enable=%d",
                             "Reread installation: Create=%d; Enable=%d"),
                          s.retry_create_status, s.retry_enable_status);
            line(buffer);
        }
        std::snprintf(buffer, sizeof(buffer),
                      tr("本次延迟关闭：%llu；纹理恢复：%llu",
                         "Session deferred closes: %llu; texture recoveries: %llu"),
                      static_cast<unsigned long long>(s.deferred_closes),
                      static_cast<unsigned long long>(s.texture_recoveries));
        line(buffer);
        if (s.resource_state >= 3 && s.resource_state <= 6) {
            std::snprintf(buffer, sizeof(buffer),
                          tr("纹理安装记录：Create=%d；Enable=%d",
                             "Texture installation: Create=%d; Enable=%d"),
                          s.resource_create_status, s.resource_enable_status);
            line(buffer);
        }
        if (!s.lifecycle_available || s.file_state >= 3 || s.resource_state >= 3 ||
            s.retry_state >= 3 || s.retry_failed != 0) {
            line(tr(
                "故障详情：data/DStorageFileLifetimeGuard/failures.log（历史与 events.log 对齐）",
                "Failure details: data/DStorageFileLifetimeGuard/failures.log (history matches "
                "events.log)"));
        }
        unwrap_();
        pop_();
    }

  private:
    using ContextFn = void *(*)();
    using TreeFn = bool (*)(const char *);
    using TextFn = void (*)(const char *, const char *);
    using WrapFn = void (*)(float);
    using VoidFn = void (*)();
    using ComboFn = bool (*)(const char *, int *, const char *const[], int, int);
    void line(const char *value) const noexcept {
        text_(value, nullptr);
    }
    void label(const char *name, const char *value) const noexcept {
        char buffer[384]{};
        std::snprintf(buffer, sizeof(buffer), "%s%s%s", name,
                      settings_.language() == GuardLanguage::English ? ": " : "：", value);
        line(buffer);
    }
    ContextFn context_{};
    TreeFn tree_{};
    TextFn text_{};
    WrapFn wrap_{};
    VoidFn pop_{};
    VoidFn unwrap_{};
    VoidFn separator_{};
    ComboFn combo_{};
    WrapFn width_{};
    GuardUiSettings settings_{};
    bool ready_{};
};

}  
