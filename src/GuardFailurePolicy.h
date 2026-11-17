// 故障分类和固定报告限额；达到报告上限不停止实际保护。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dstorage_guard {

struct ProtectionFailureState {
    bool lifecycle{};
    std::uint32_t file{}, resource{}, retry{}, iat{}, reporter{};
};

// Pending factory/installation states are not failed protections.
constexpr std::uint32_t protection_failure_mask(const ProtectionFailureState &s) {
    std::uint32_t mask = s.lifecycle ? 0U : 1U;
    if (s.lifecycle) {
        if (s.file >= 3 || (s.file == 0 && s.iat != 1)) {
            mask |= 2U;
        }
        if (s.resource >= 3 || (s.file == 1 && s.resource == 0)) {
            mask |= 4U;
        }
        if (s.retry >= 3 || (s.file == 1 && s.retry == 0)) {
            mask |= 8U;
        }
    }
    if (s.reporter != 3) {
        mask |= 16U;
    }
    return mask;
}

enum class DiagnosticKind : std::size_t { install, resource, retry, counters, count };

// Owned exclusively by the existing background writer (startup fallback only
// when that writer could not be created). Limits affect diagnostics, not repair.
class FailureBudget {
  public:
    static constexpr unsigned limit = 16;
    bool admit(DiagnosticKind kind) {
        auto &count = counts_[static_cast<std::size_t>(kind)];
        if (count >= limit + 1) {
            return false;
        }
        return ++count <= limit;
    }
    bool take_limit_notice(DiagnosticKind kind) {
        const auto index = static_cast<std::size_t>(kind);
        if (counts_[index] != limit + 1 || noticed_[index]) {
            return false;
        }
        noticed_[index] = true;
        return true;
    }

  private:
    std::array<unsigned, static_cast<std::size_t>(DiagnosticKind::count)> counts_{};
    std::array<bool, static_cast<std::size_t>(DiagnosticKind::count)> noticed_{};
};

} // namespace dstorage_guard
