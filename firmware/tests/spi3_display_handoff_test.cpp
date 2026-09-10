#include "hal/board/spi3_display_handoff.h"

#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct FakeBackend {
    std::vector<std::string> events;
    bool drain_succeeds = true;
    bool input_succeeds = true;
    bool cs_succeeds = true;
};

bool check(bool condition, const char* expression) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "check failed: %s\n", expression);
    return false;
}

void lock_display(void* context) {
    static_cast<FakeBackend*>(context)->events.emplace_back("lock");
}

bool drain_display(void* context, std::string& error) {
    auto& backend = *static_cast<FakeBackend*>(context);
    backend.events.emplace_back("drain");
    if (!backend.drain_succeeds) {
        error = "drain failed";
        return false;
    }
    return true;
}

bool set_shared_pin_input(void* context, std::string& error) {
    auto& backend = *static_cast<FakeBackend*>(context);
    backend.events.emplace_back("input");
    if (!backend.input_succeeds) {
        error = "input failed";
        return false;
    }
    return true;
}

bool set_sd_cs_high(void* context, std::string& error) {
    auto& backend = *static_cast<FakeBackend*>(context);
    backend.events.emplace_back("cs_high");
    if (!backend.cs_succeeds) {
        error = "cs failed";
        return false;
    }
    return true;
}

void unlock_display(void* context) {
    static_cast<FakeBackend*>(context)->events.emplace_back("unlock");
}

board::Spi3DisplayHandoff make_handoff(FakeBackend& backend) {
    return board::Spi3DisplayHandoff({&backend, lock_display, drain_display, set_shared_pin_input,
                                      set_sd_cs_high, unlock_display});
}

bool events_equal(const std::vector<std::string>& actual, std::initializer_list<const char*> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    size_t index = 0;
    for (const char* event : expected) {
        if (actual[index++] != event) {
            return false;
        }
    }
    return true;
}

bool test_acquire_and_release_order() {
    FakeBackend backend;
    auto handoff = make_handoff(backend);

    if (!check(handoff.acquire(), "handoff acquires")) {
        return false;
    }
    handoff.release();

    return check(events_equal(backend.events, {"lock", "drain", "input", "cs_high", "unlock"}),
                 "handoff follows safe pin order");
}

bool test_drain_failure_unlocks_without_touching_sd_pin() {
    FakeBackend backend;
    backend.drain_succeeds = false;
    auto handoff = make_handoff(backend);

    return check(!handoff.acquire(), "drain failure rejects acquire") &&
           check(handoff.last_error() == "drain failed", "drain error is preserved") &&
           check(events_equal(backend.events, {"lock", "drain", "unlock"}),
                 "drain failure unlocks immediately");
}

bool test_input_failure_restores_safe_cs_and_unlocks() {
    FakeBackend backend;
    backend.input_succeeds = false;
    auto handoff = make_handoff(backend);

    return check(!handoff.acquire(), "input failure rejects acquire") &&
           check(handoff.last_error() == "input failed", "input error is preserved") &&
           check(events_equal(backend.events, {"lock", "drain", "input", "cs_high", "unlock"}),
                 "input failure leaves SD deselected before unlock");
}

bool test_input_and_cs_failure_keeps_display_locked_until_guard_retry() {
    FakeBackend backend;
    backend.input_succeeds = false;
    backend.cs_succeeds = false;
    auto handoff = make_handoff(backend);
    board::Spi3DisplayHandoffGuard guard(handoff);

    if (!check(!guard.acquired(), "input failure does not grant SD access") ||
        !check(handoff.is_acquired(), "failed deselect keeps the display handoff active") ||
        !check(events_equal(backend.events, {"lock", "drain", "input", "cs_high"}),
               "input and chip-select failure keeps the display locked")) {
        return false;
    }

    backend.cs_succeeds = true;
    return check(guard.release(), "guard retries cleanup after failed acquire") &&
           check(!handoff.is_acquired(), "successful cleanup retry ends failed handoff") &&
           check(events_equal(backend.events, {"lock", "drain", "input", "cs_high", "cs_high", "unlock"}),
                 "failed acquire unlocks only after SD deselect succeeds");
}

bool test_guard_releases_exactly_once_after_move() {
    FakeBackend backend;
    auto handoff = make_handoff(backend);

    {
        board::Spi3DisplayHandoffGuard first(handoff);
        if (!check(first.acquired(), "first guard acquires")) {
            return false;
        }
        board::Spi3DisplayHandoffGuard second(std::move(first));
        if (!check(!first.acquired(), "moved-from guard relinquishes ownership") ||
            !check(second.acquired(), "moved-to guard owns release")) {
            return false;
        }
    }

    return check(events_equal(backend.events, {"lock", "drain", "input", "cs_high", "unlock"}),
                 "moved guard releases once");
}

bool test_nested_acquire_is_rejected_without_double_release() {
    FakeBackend backend;
    auto handoff = make_handoff(backend);

    {
        board::Spi3DisplayHandoffGuard outer(handoff);
        board::Spi3DisplayHandoffGuard nested(handoff);
        if (!check(outer.acquired(), "outer guard acquires") ||
            !check(!nested.acquired(), "nested guard is rejected")) {
            return false;
        }
    }

    return check(events_equal(backend.events, {"lock", "drain", "input", "cs_high", "unlock"}),
                 "nested guard does not unlock outer guard");
}

bool test_cs_failure_keeps_display_locked_until_retry_succeeds() {
    FakeBackend backend;
    backend.cs_succeeds = false;
    auto handoff = make_handoff(backend);
    board::Spi3DisplayHandoffGuard guard(handoff);

    if (!check(guard.acquired(), "guard acquires before release failure") ||
        !check(!guard.release(), "guard reports chip-select release failure") ||
        !check(handoff.last_error() == "cs failed", "chip-select error is preserved") ||
        !check(handoff.is_acquired(), "handoff remains acquired after chip-select failure") ||
        !check(guard.acquired(), "guard remains active so release can be retried") ||
        !check(events_equal(backend.events, {"lock", "drain", "input", "cs_high"}),
               "chip-select failure keeps the display locked")) {
        return false;
    }

    backend.cs_succeeds = true;
    return check(guard.release(), "retry succeeds after chip-select is restored") &&
           check(!handoff.is_acquired(), "successful retry ends the handoff") &&
           check(!guard.acquired(), "successful retry deactivates the guard") &&
           check(events_equal(backend.events, {"lock", "drain", "input", "cs_high", "cs_high", "unlock"}),
                 "only a successful chip-select restore unlocks the display");
}

bool test_persistent_cs_failure_is_fail_closed_on_guard_destruction() {
    FakeBackend backend;
    backend.cs_succeeds = false;
    auto handoff = make_handoff(backend);

    {
        board::Spi3DisplayHandoffGuard guard(handoff);
        if (!check(guard.acquired(), "guard acquires before persistent release failure")) {
            return false;
        }
    }

    return check(handoff.is_acquired(), "persistent chip-select failure keeps handoff acquired") &&
           check(events_equal(backend.events, {"lock", "drain", "input", "cs_high"}),
                 "guard destruction leaves the display locked when SD cannot be deselected");
}

bool test_move_assignment_does_not_abandon_failed_release() {
    FakeBackend first_backend;
    first_backend.cs_succeeds = false;
    FakeBackend second_backend;
    auto first_handoff = make_handoff(first_backend);
    auto second_handoff = make_handoff(second_backend);
    board::Spi3DisplayHandoffGuard first(first_handoff);
    board::Spi3DisplayHandoffGuard second(second_handoff);

    first = std::move(second);

    if (!check(first_handoff.is_acquired(), "failed move-assignment release keeps first handoff acquired") ||
        !check(second_handoff.is_acquired(), "failed move assignment leaves source ownership intact") ||
        !check(first.acquired(), "destination guard keeps failed-release ownership") ||
        !check(second.acquired(), "source guard remains active after rejected move assignment") ||
        !check(events_equal(first_backend.events, {"lock", "drain", "input", "cs_high"}),
               "failed move-assignment release keeps first display locked")) {
        return false;
    }

    first_backend.cs_succeeds = true;
    return check(first.release(), "first guard can retry after rejected move assignment") &&
           check(second.release(), "source guard can still release normally");
}

static_assert(!std::is_copy_constructible_v<board::Spi3DisplayHandoffGuard>);
static_assert(!std::is_copy_assignable_v<board::Spi3DisplayHandoffGuard>);
static_assert(std::is_nothrow_move_constructible_v<board::Spi3DisplayHandoffGuard>);
static_assert(std::is_nothrow_move_assignable_v<board::Spi3DisplayHandoffGuard>);

}  // namespace

int main() {
    int failures = 0;
    failures += !test_acquire_and_release_order();
    failures += !test_drain_failure_unlocks_without_touching_sd_pin();
    failures += !test_input_failure_restores_safe_cs_and_unlocks();
    failures += !test_input_and_cs_failure_keeps_display_locked_until_guard_retry();
    failures += !test_guard_releases_exactly_once_after_move();
    failures += !test_nested_acquire_is_rejected_without_double_release();
    failures += !test_cs_failure_keeps_display_locked_until_retry_succeeds();
    failures += !test_persistent_cs_failure_is_fail_closed_on_guard_destruction();
    failures += !test_move_assignment_does_not_abandon_failed_release();
    return failures == 0 ? 0 : 1;
}
