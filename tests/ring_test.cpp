// The ring to the strategy (note 002, S4): with the ring full the feed
// thread never waits; each instrument's events arrive in order, its last is
// its newest, and every event replaced aside is counted. On one thread with
// nothing read, then with a consumer thread reading slower than the feed
// pushes (ThreadSanitizer runs it in CI).
// Spec: docs/design/002-book-events-strategy.md
#include "check.hpp"

#include <ttt/event_ring.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ttt;
using ttt::test::check;

book_event ev(std::uint16_t slot, std::uint64_t seq) {
    book_event e;
    e.slot = slot;
    e.locate = static_cast<std::uint16_t>(100 + slot);
    e.sequence = seq;
    e.bid_price = static_cast<std::uint32_t>(seq);
    return e;
}

void one_thread() {
    auto r = std::make_unique<event_ring<16>>();
    constexpr std::uint16_t slots = 5;
    std::uint64_t seq = 0;
    for (int i = 0; i < 100; ++i) r->push(ev(static_cast<std::uint16_t>(i % slots), ++seq));
    const std::uint64_t in_ring = event_ring<16>::ring::capacity;
    check(r->waiting() == slots && r->set_aside() == slots && r->replaced() == 100 - in_ring - slots,
          "nothing read: 100 pushes return; 16 in the ring, one aside per instrument (" + std::to_string(r->waiting()) + "), " +
              std::to_string(r->replaced()) + " replaced and counted");
    std::vector<std::uint64_t> last(slots, 0);
    bool in_order = true;
    std::uint64_t popped = 0;
    book_event e;
    for (;;) {
        while (r->pop(e)) {
            ++popped;
            in_order &= e.sequence > last[e.slot];
            last[e.slot] = e.sequence;
        }
        if (r->waiting() == 0) break;
        r->flush();
    }
    bool newest = true;
    for (std::uint16_t s = 0; s < slots; ++s) newest &= last[s] == 100u - (slots - 1u - s);
    check(in_order && newest && popped + r->replaced() == 100,
          "read after: each instrument in order, its last event its newest, read + replaced = pushed");

    // Ring room again: straight through, nothing aside.
    const std::uint64_t before = r->set_aside();
    r->push(ev(0, ++seq));
    check(r->pop(e) && e.sequence == seq && r->set_aside() == before, "with room and nothing aside: straight into the ring");
}

void two_threads() {
    auto r = std::make_unique<event_ring<64>>();
    constexpr std::uint16_t slots = 8;
    constexpr std::uint64_t n = 2'000'000;
    std::atomic<bool> done{false};
    std::vector<std::uint64_t> last(slots, 0);
    std::uint64_t popped = 0;
    bool in_order = true;
    std::thread consumer([&] {
        book_event e;
        for (;;) {
            if (r->pop(e)) {
                ++popped;
                in_order &= e.sequence > last[e.slot];
                last[e.slot] = e.sequence;
                for (int spin = 0; spin < 20; ++spin) asm volatile("");  // slower than the feed
            } else if (done.load(std::memory_order_acquire)) {
                if (!r->pop(e)) break;
                ++popped;
                in_order &= e.sequence > last[e.slot];
                last[e.slot] = e.sequence;
            }
        }
    });
    std::vector<std::uint64_t> newest(slots, 0);
    for (std::uint64_t i = 1; i <= n; ++i) {
        const auto s = static_cast<std::uint16_t>(i * 7 % slots);
        r->push(ev(s, i));
        newest[s] = i;
    }
    while (r->waiting() > 0) r->flush();  // the feed thread's ticks after the last message
    done.store(true, std::memory_order_release);
    consumer.join();
    check(in_order && last == newest && popped + r->replaced() == n,
          "a slower consumer thread: " + std::to_string(n) + " pushed, " + std::to_string(r->replaced()) +
              " replaced; each instrument in order, its last its newest, read + replaced = pushed");
    check(r->replaced() > 0, "the ring was full at times (so the aside path ran)");
}

}  // namespace

int main() {
    one_thread();
    two_threads();
    return ttt::test::result();
}
