#include "../src/GuardFailurePolicy.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks{};
static void check(bool ok) { ++checks; if (!ok) { std::printf("FAIL check=%u\n", checks); std::exit(1); } }
int main() {
    using namespace dstorage_guard;
    const ProtectionFailureState healthy{true, 1, 1, 2, 0, 3};
    check(protection_failure_mask(healthy) == 0);
    check(protection_failure_mask({true, 0, 0, 0, 1, 3}) == 0);
    check(protection_failure_mask({true, 2, 2, 1, 0, 3}) == 0);
    check(protection_failure_mask({false, 0, 0, 0, 0, 3}) == 1);
    check(protection_failure_mask({true, 1, 1, 2, 0, 0}) == 16);
    check(protection_failure_mask({true, 1, 1, 0, 0, 3}) == 8);
    for (unsigned state = 3; state <= 7; ++state) {
        auto s = healthy; s.file = state; check(protection_failure_mask(s) == 2);
        s = healthy; s.resource = state; check(protection_failure_mask(s) == 4);
        s = healthy; s.retry = state; check(protection_failure_mask(s) == 8);
    }
    FailureBudget budget;
    for (unsigned k = 0; k < static_cast<unsigned>(DiagnosticKind::count); ++k) {
        const auto kind = static_cast<DiagnosticKind>(k);
        for (unsigned i = 0; i < 16; ++i) { check(budget.admit(kind)); check(!budget.take_limit_notice(kind)); }
        check(!budget.admit(kind)); check(budget.take_limit_notice(kind));
        for (unsigned i = 0; i < 100000; ++i) { check(!budget.admit(kind)); check(!budget.take_limit_notice(kind)); }
    }
    std::printf("failure_policy=PASS checks=%u healthy_path_no_report=1 bounded_failure_storm=1\n", checks);
}
