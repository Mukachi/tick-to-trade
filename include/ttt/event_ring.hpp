// The ring from the feed thread to the strategy (note 002, D4 A, note 001's
// arrangement A): lockfree-ipc's SPSC ring of book events, and on the feed
// thread's side a place aside for each instrument. The feed thread never
// waits: an event that finds the ring full, or finds events already aside,
// is kept aside, replacing the instrument's event kept there (counted: a
// book event is a state, so only the newest matters). What is aside goes
// first, oldest instrument first, whenever there is room: at the next push,
// or at flush() (the feed thread's tick). An instrument with an event aside
// never has a newer one pushed past it.
// Design: docs/design/002-book-events-strategy.md
#pragma once

#include <ttt/events.hpp>

#include <lfipc/spsc.hpp>

#include <cstdint>
#include <memory>

namespace ttt {

template <std::size_t Capacity = 4096>
class event_ring {
public:
    using ring = lfipc::spsc_ring<book_event, Capacity>;

    event_ring() : ring_(std::make_unique<ring>()) {}

    // ---- the feed thread's side
    void push(const book_event& e) {
        flush();
        aside& a = aside_[e.slot];
        if (a.present) {
            a.e = e;
            ++replaced_;
            return;
        }
        if (count_ == 0 && ring_->try_push(e)) return;
        a.e = e;
        a.present = true;
        order_[(head_ + count_++) % max_instruments] = e.slot;
        ++set_aside_;
    }
    void flush() {
        while (count_ > 0) {
            aside& a = aside_[order_[head_]];
            if (!ring_->try_push(a.e)) return;
            a.present = false;
            head_ = (head_ + 1) % max_instruments;
            --count_;
        }
    }
    std::uint64_t replaced() const { return replaced_; }   // events dropped for a newer one of the same instrument
    std::uint64_t set_aside() const { return set_aside_; } // events that found the ring full
    std::size_t waiting() const { return count_; }          // instruments with an event aside now

    // ---- the strategy's side
    bool pop(book_event& e) { return ring_->try_pop(e); }

private:
    struct aside {
        book_event e;
        bool present = false;
    };
    std::unique_ptr<ring> ring_;
    aside aside_[max_instruments];
    std::uint16_t order_[max_instruments] = {};
    std::size_t head_ = 0, count_ = 0;
    std::uint64_t replaced_ = 0, set_aside_ = 0;
};

}  // namespace ttt
