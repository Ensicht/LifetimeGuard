#pragma once
#include <cstring>
#include <string>
#include <vector>

namespace mock_guard_ui {
inline bool expanded = true;
inline int context_token{};
inline unsigned context_calls{}, tree_calls{}, pops{}, wraps{}, unwraps{}, separators{};
inline int requested_language = -1;
inline unsigned combo_calls{};
inline std::vector<std::string> lines;
inline void reset() {
    context_calls = tree_calls = pops = wraps = unwraps = separators = 0;
    combo_calls = 0;
    lines.clear();
}
inline bool contains(const char* value) {
    for (const auto& line : lines) {
        if (line.find(value) != std::string::npos) { return true; }
    }
    return false;
}
}

extern "C" __declspec(dllexport) void* igGetCurrentContext() {
    ++mock_guard_ui::context_calls;
    return &mock_guard_ui::context_token;
}
extern "C" __declspec(dllexport) bool igTreeNode_Str(const char* label) {
    ++mock_guard_ui::tree_calls;
    mock_guard_ui::lines.emplace_back(label);
    return mock_guard_ui::expanded;
}
extern "C" __declspec(dllexport) void igTreePop() { ++mock_guard_ui::pops; }
extern "C" __declspec(dllexport) void igTextUnformatted(const char* begin, const char* end) {
    mock_guard_ui::lines.emplace_back(begin, end ? static_cast<std::size_t>(end - begin) : std::strlen(begin));
}
extern "C" __declspec(dllexport) void igPushTextWrapPos(float) { ++mock_guard_ui::wraps; }
extern "C" __declspec(dllexport) void igPopTextWrapPos() { ++mock_guard_ui::unwraps; }
extern "C" __declspec(dllexport) void igSeparator() { ++mock_guard_ui::separators; }

extern "C" __declspec(dllexport) void igSetNextItemWidth(float) {}
extern "C" __declspec(dllexport) bool igCombo_Str_arr(const char* label, int* selected,
    const char* const[], int count, int) {
    ++mock_guard_ui::combo_calls;
    mock_guard_ui::lines.emplace_back(label);
    if (mock_guard_ui::requested_language < 0 || mock_guard_ui::requested_language >= count) { return false; }
    const int value = mock_guard_ui::requested_language;
    mock_guard_ui::requested_language = -1;
    const bool changed = *selected != value;
    *selected = value;
    return changed;
}
