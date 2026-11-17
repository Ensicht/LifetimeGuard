#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <string>
#include "../src/GuardStatusUi.h"
#include "MockGuardStatusHost.h"

int main() {
    dstorage_guard::GuardStatusUi ui;
    if (!ui.load(GetModuleHandleW(nullptr))) { return 1; }
    dstorage_guard::GuardStatusSnapshot s{};
    unsigned failures{}, cases{};
    const auto check = [&](const char* expected) {
        mock_guard_ui::reset();
        ui.draw(&mock_guard_ui::context_token, s);
        const bool ok = mock_guard_ui::contains(expected) && mock_guard_ui::pops == 1 &&
            mock_guard_ui::wraps == 1 && mock_guard_ui::unwraps == 1;
        std::printf("ui_case=%u passed=%d\n", ++cases, ok);
        failures += !ok;
    };
    check("正在初始化");
    s.initialize_state = 2;
    check("加载生命周期不可用");
    s.lifecycle_available = true;
    s.iat_state = 1;
    check("等待 DirectStorage 初始化");
    s.file_state = 2;
    check("正在安装文件保护");
    s.file_state = 5;
    check("保护安装失败");
    s.file_state = 1;
    s.resource_state = 5;
    s.resource_create_status = 9;
    check("保护不完整：仅文件保护");
    check("Create=9");
    for (unsigned state = 3; state <= 7; ++state) {
        s.resource_state = state;
        check(dstorage_guard::guard_layer_state(state));
    }
    s.resource_state = 1;
    s.resource_mode = 1;
    check("纹理重读尚未就绪");
    s.retry_state = 5;
    s.retry_create_status = 9;
    check("重读安装记录：Create=9");
    s.retry_state = 2;
    check("文件、纹理收尾与重读保护均已就绪");
    check("原生函数 Hook");
    s.resource_mode = 2;
    check("任务表备用入口");
    s.loading = true;
    check("加载中，保护已启用");
    s.loading = false;
    s.active_leases = 17;
    check("非加载期，停止接收新保护请求");
    check("正在排空的文件请求：17");
    s.gameplay_retry = true;
    s.retry_inflight = 4;
    s.retry_backoffs = 12;
    check("非加载期，保留失败纹理重读与清理配对");
    check("松散纹理清理配对");
    check("未启用，请查看故障报告");
    s.loose_cleanup_ready = true;
    s.loose_cleanup_pairs = 17;
    check("已就绪（仅原生清理事件）");
    check("本次清理引用配对：17");
    check("重读在途：4");
    check("退避：12");
    s.retry_events_dropped = 7;
    check("省略事件：7");

    wchar_t settings_path[MAX_PATH]{}, relative_path[MAX_PATH]{};
    if (!GetTempFileNameW(L".", L"lgu", 0, relative_path)) { return 3; }
    const auto path_size = GetFullPathNameW(relative_path, MAX_PATH, settings_path, nullptr);
    if (path_size == 0 || path_size >= MAX_PATH) { DeleteFileW(relative_path); return 3; }
    DeleteFileW(settings_path);
    ui.configure_settings(settings_path);
    check("语言 / Language");
    mock_guard_ui::requested_language = 1;
    check("File, texture completion and reread protections ready");
    check("Texture entry: Task table fallback");
    check("Rereads in flight: 4; gameplay submissions: 0; backoffs: 12");
    check("Diagnostic buffer full, omitted events: 7");
    dstorage_guard::GuardUiSettings saved;
    saved.load(settings_path);
    failures += saved.language() != dstorage_guard::GuardLanguage::English;
    ui.configure_settings(settings_path);
    check("Status: gameplay, failed-texture rereads and cleanup pairing");
    check("Loose-texture cleanup pairing");
    check("Ready (native cleanup events only)");
    check("Cleanup reference pairs: 17");
    s.loading = true;
    check("Status: loading, protection active");
    for (unsigned state = 0; state <= 8; ++state) {
        s.resource_state = state;
        check(dstorage_guard::guard_layer_state(state, dstorage_guard::GuardLanguage::English));
    }
    s.resource_state = 5;
    check("Texture installation: Create=9");
    s.retry_state = 5;
    check("Reread installation: Create=9");
    s.lifecycle_available = false;
    check("Protection unavailable: loading lifecycle missing");
    s.lifecycle_available = true;
    s.file_state = 5;
    check("Protection installation failed");
    s.file_state = 2;
    check("Installing file protection");
    s.file_state = 0;
    check("Waiting for DirectStorage initialization");
    s.initialize_state = 0;
    check("Initializing");
    s.initialize_state = 2;
    s.file_state = s.resource_state = 1;
    s.retry_state = 2;
    mock_guard_ui::requested_language = 0;
    check("文件、纹理收尾与重读保护均已就绪");
    saved.load(settings_path);
    failures += saved.language() != dstorage_guard::GuardLanguage::Chinese;
    WritePrivateProfileStringW(L"UI", L"language", L"invalid", settings_path);
    saved.load(settings_path);
    failures += saved.language() != dstorage_guard::GuardLanguage::Chinese;
    WritePrivateProfileStringW(L"Other", L"keep", L"yes", settings_path);
    saved.set_language(dstorage_guard::GuardLanguage::English);
    wchar_t retained[8]{};
    GetPrivateProfileStringW(L"Other", L"keep", L"", retained, 8, settings_path);
    failures += std::wcscmp(retained, L"yes") != 0 || saved.save_failed();
    saved.set_language(static_cast<dstorage_guard::GuardLanguage>(9));
    failures += saved.language() != dstorage_guard::GuardLanguage::English;
    const auto blocked = std::wstring(settings_path) + L"\\blocked.ini";
    ui.configure_settings(blocked.c_str());
    mock_guard_ui::requested_language = 1;
    check("Language could not be saved");
    check("File, texture completion and reread protections ready");
    ui.configure_settings(settings_path);
    DeleteFileW(settings_path);
    mock_guard_ui::reset();
    for (unsigned i = 0; i < 10000; ++i) { ui.draw(&mock_guard_ui::context_token, s); }
    failures += GetFileAttributesW(settings_path) != INVALID_FILE_ATTRIBUTES;
    failures += mock_guard_ui::combo_calls != 10000;
    mock_guard_ui::expanded = false;
    mock_guard_ui::reset();
    for (unsigned i = 0; i < 100000; ++i) { ui.draw(&mock_guard_ui::context_token, s); }
    failures += mock_guard_ui::pops != 0 || mock_guard_ui::wraps != 0 ||
        mock_guard_ui::tree_calls != 100000 || mock_guard_ui::combo_calls != 0;
    mock_guard_ui::reset();
    ui.draw(nullptr, s);
    ui.draw(&s, s);
    failures += mock_guard_ui::tree_calls != 0;
    if (ui.load(GetModuleHandleW(L"kernel32.dll"))) { ++failures; }
    mock_guard_ui::reset();
    ui.draw(&mock_guard_ui::context_token, s);
    failures += mock_guard_ui::context_calls != 0;
    if (ui.load(nullptr)) { ++failures; }
    std::printf("status_ui_cases=%u collapsed_draws=100000 failures=%u\n", cases, failures);
    return failures == 0 ? 0 : 2;
}
