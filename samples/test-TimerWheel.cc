#include <algorithm>
#include <functional>
#include <vector>
#include <cstdio>
#include <godby/TimerWheel.h>

using namespace godby;

#define TEST(fun) \
    do {                                              \
        if (fun()) {                                  \
            printf("[OK] %s\n", #fun);                \
        } else {                                      \
            ok = false;                               \
            printf("[FAILED] %s\n", #fun);            \
        }                                             \
    } while (0)

#define EXPECT(expr)                                    \
    do {                                                \
        if (!(expr))  {                                 \
            printf("%s:%d: Expect failed: %s\n",        \
                   __FILE__, __LINE__, #expr);          \
            return false;                               \
        }                                               \
    } while (0)

#define EXPECT_INTEQ(actual, expect)                    \
    do {                                                \
        if (expect != actual)  {                        \
            printf("%s:%d: Expect failed, wanted %ld"   \
                   " got %ld\n",                        \
                   __FILE__, __LINE__,                  \
                   (long) expect, (long) actual);       \
            return false;                               \
        }                                               \
    } while (0)

bool test_single_timer_no_hierarchy() {
    using Callback = std::function<void()>;
    TimerWheel timers;
    int count = 0;
    CallbackTimerEvent<Callback> timer([&count] () { ++count; });

    // Unscheduled timer does nothing.
    timers.advance(10);
    EXPECT_INTEQ(count, 0);
    EXPECT(!timer.scheduled());

    // Schedule timer, should trigger at right time.
    timers.schedule(&timer, 5);
    EXPECT(timer.scheduled());
    timers.advance(5);
    EXPECT_INTEQ(count, 1);

    // Only trigger once, not repeatedly (even if wheel wraps
    // around).
    timers.advance(256);
    EXPECT_INTEQ(count, 1);

    // ... unless, of course, the timer gets scheduled again.
    timers.schedule(&timer, 5);
    timers.advance(5);
    EXPECT_INTEQ(count, 2);

    // Canceled timers don't run.
    timers.schedule(&timer, 5);
    timer.cancel();
    EXPECT(!timer.scheduled());
    timers.advance(10);
    EXPECT_INTEQ(count, 2);

    // Test wraparound
    timers.advance(250);
    timers.schedule(&timer, 5);
    timers.advance(10);
    EXPECT_INTEQ(count, 3);

    // Timers that are scheduled multiple times only run at the last
    // scheduled tick.
    timers.schedule(&timer, 5);
    timers.schedule(&timer, 10);
    timers.advance(5);
    EXPECT_INTEQ(count, 3);
    timers.advance(5);
    EXPECT_INTEQ(count, 4);

    // Timer can safely be canceled multiple times.
    timers.schedule(&timer, 5);
    timer.cancel();
    timer.cancel();
    EXPECT(!timer.scheduled());
    timers.advance(10);
    EXPECT_INTEQ(count, 4);

    {
        CallbackTimerEvent<Callback> timer2([&count] () { ++count; });
        timers.schedule(&timer2, 5);
    }
    timers.advance(10);
    EXPECT_INTEQ(count, 4);

    return true;
}

bool test_single_timer_hierarchy() {
    using Callback = std::function<void()>;
    TimerWheel timers;
    int count = 0;
    CallbackTimerEvent<Callback> timer([&count] () { ++count; });

    EXPECT_INTEQ(count, 0);

    // Schedule timer one layer up (make sure timer ends up in slot 0 once
    // promoted to the innermost wheel, since that's a special case).
    timers.schedule(&timer, 256);
    timers.advance(255);
    EXPECT_INTEQ(count, 0);
    timers.advance(1);
    EXPECT_INTEQ(count, 1);

    // Then schedule one that ends up in some other slot
    timers.schedule(&timer, 257);
    timers.advance(256);
    EXPECT_INTEQ(count, 1);
    timers.advance(1);
    EXPECT_INTEQ(count, 2);

    // Schedule multiple rotations ahead in time, to slot 0.
    timers.schedule(&timer, 256 * 4 - 1);
    timers.advance(256 * 4 - 2);
    EXPECT_INTEQ(count, 2);
    timers.advance(1);
    EXPECT_INTEQ(count, 3);

    // Schedule multiple rotations ahead in time, to non-0 slot. (Do this
    // twice, once starting from slot 0, once starting from slot 5);
    for (int i = 0; i < 2; ++i) {
        timers.schedule(&timer, 256 * 4 + 5);
        timers.advance(256 * 4 + 4);
        EXPECT_INTEQ(count, 3 + i);
        timers.advance(1);
        EXPECT_INTEQ(count, 4 + i);
    }

    return true;
}

bool test_ticks_to_next_event() {
    using Callback = std::function<void()>;
    TimerWheel timers;
    CallbackTimerEvent<Callback> timer([] () { });
    CallbackTimerEvent<Callback> timer2([] () { });

    // No timers scheduled, return the max value.
    EXPECT_INTEQ(timers.ticks_to_wakeup(100), 100);
    EXPECT_INTEQ(timers.ticks_to_wakeup(), std::numeric_limits<TickType>::max());

    for (int i = 0; i < 10; ++i) {
        // Just vanilla tests
        timers.schedule(&timer, 1);
        EXPECT_INTEQ(timers.ticks_to_wakeup(100), 1);

        timers.schedule(&timer, 20);
        EXPECT_INTEQ(timers.ticks_to_wakeup(100), 20);

        // Check the the "max" parameters works.
        timers.schedule(&timer, 150);
        EXPECT_INTEQ(timers.ticks_to_wakeup(100), 100);

        // Check that a timer on the next layer can be found.
        timers.schedule(&timer, 280);
        EXPECT_INTEQ(timers.ticks_to_wakeup(100), 100);
        EXPECT_INTEQ(timers.ticks_to_wakeup(1000), 280);

        for (int i = 1; i < 256; ++i) {
            timers.schedule(&timer2, i);
            EXPECT_INTEQ(timers.ticks_to_wakeup(1000), i);
        }

        timer.cancel();
        timer2.cancel();
        timers.advance(32);
    }

    for (int i = 0; i < 20; ++i) {
        timers.schedule(&timer, 270);
        timers.advance(128);
        EXPECT_INTEQ(timers.ticks_to_wakeup(512), 270 - 128);
        timers.schedule(&timer2, 250);
        EXPECT_INTEQ(timers.ticks_to_wakeup(512), 270 - 128);
        timers.schedule(&timer2, 10);
        EXPECT_INTEQ(timers.ticks_to_wakeup(512), 10);
        timers.advance(32);
    }

    timer.cancel();
    EXPECT_INTEQ(timers.ticks_to_wakeup(), std::numeric_limits<TickType>::max());

    return true;
}

// ... other test functions follow the same pattern ...

int main(void) {
    bool ok = true;
    TEST(test_single_timer_no_hierarchy);
    TEST(test_single_timer_hierarchy);
    TEST(test_ticks_to_next_event);
    // ... other test cases ...
    return ok ? 0 : 1;
}
