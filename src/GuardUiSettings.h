 
#pragma once

#include <windows.h>
#include <cwchar>

namespace dstorage_guard {

enum class GuardLanguage { Chinese, English };

inline const char *localized(GuardLanguage language, const char *chinese,
                             const char *english) noexcept {
    return language == GuardLanguage::English ? english : chinese;
}

 
class GuardUiSettings {
  public:
    void load(const wchar_t *path) noexcept {
        language_ = GuardLanguage::Chinese;
        save_failed_ = false;
        path_[0] = L'\0';
        if (path == nullptr || *path == L'\0') {
            return;
        }
        const auto size = GetFullPathNameW(path, MAX_PATH, path_, nullptr);
        if (size == 0 || size >= MAX_PATH) {
            path_[0] = L'\0';
            return;
        }
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"UI", L"language", L"zh", value, 16, path_);
        if (std::wcscmp(value, L"en") == 0) {
            language_ = GuardLanguage::English;
        }
    }

    GuardLanguage language() const noexcept {
        return language_;
    }
    bool save_failed() const noexcept {
        return save_failed_;
    }

    void set_language(GuardLanguage language) noexcept {
        if (language != GuardLanguage::Chinese && language != GuardLanguage::English) {
            return;
        }
        if (language_ == language) {
            return;
        }
        language_ = language;
        if (path_[0] == L'\0') {
            save_failed_ = true;
            return;
        }
        wchar_t directory[MAX_PATH]{};
        std::wcscpy(directory, path_);
        if (auto *slash = std::wcsrchr(directory, L'\\')) {
            *slash = L'\0';
            CreateDirectoryW(directory, nullptr);
        }
        save_failed_ = WritePrivateProfileStringW(
                           L"UI", L"language", language == GuardLanguage::English ? L"en" : L"zh",
                           path_) == FALSE;
    }

  private:
    wchar_t path_[MAX_PATH]{};
    GuardLanguage language_{GuardLanguage::Chinese};
    bool save_failed_{};
};

}  
