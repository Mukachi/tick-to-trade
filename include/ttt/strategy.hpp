// The strategy (note 001, D2; note 002): quote following. For each traded
// instrument, at most one buy order of one round lot, resting at the best
// bid, and at most one request for it in flight:
//   no order           a usable event with a new best bid: new, at the bid
//   resting            a usable event with a new best bid: replace to it (D2,
//                      one OrderCancelReplaceRequest); an unusable event: cancel
//   request in flight  remember the newest bid, or to cancel; once the answer
//                      settles the state, act on what was remembered
// A new best bid (D1): its price differs from the last usable event's, or the
// event is the first usable one since an unusable one. A change only of
// shares, or of the ask, sends nothing. The strategy reads no clock and keeps
// nothing but what events and order events give it, so the same events and
// answers give the same intents. Each request's client order ID comes from a
// counter started at a configured value, the instrument's slot in its low 16
// bits (order_id below), so an answer finds its instrument without a lookup.
// Intents go to the risk checks (note 003), answers come back from the gateway.
// Design: docs/design/002-book-events-strategy.md
#pragma once

#include <ttt/events.hpp>

#include <cstdint>
#include <vector>

namespace ttt {

// A client order ID: a counter above, the instrument's slot below.
inline std::uint64_t order_id(std::uint64_t counter, std::uint16_t slot) { return counter << 16 | slot; }
inline std::uint16_t slot_of(std::uint64_t id) { return static_cast<std::uint16_t>(id & 0xFFFF); }

struct intent {
    enum kind_t : std::uint8_t { new_order = 1, replace = 2, cancel = 3 } kind;
    std::uint16_t slot;
    std::uint16_t locate;
    std::uint64_t id;       // the request's client order ID
    std::uint64_t orig_id;  // replace, cancel: the order's (its current ID)
    std::uint32_t price;    // new, replace: four implied decimals (cancel: the order's)
    std::uint32_t quantity; // the instrument's round lot
    std::uint64_t sequence; // the ITCH message that caused it (0: an order event did)
    bool operator==(const intent&) const = default;
};

// What the gateway says of an order or a request (note 004 maps its order events to these).
struct order_answer {
    enum kind_t : std::uint8_t {
        accepted = 1,  // a new order acknowledged
        replaced,      // a replace done: the order now has the request's ID and price
        cancelled,     // the order is gone: a cancel done, or the venue's own
        rejected,      // the request refused (by the risk checks, the gateway or the venue); the order as before
        filled,        // the order wholly filled: gone
        partly_filled, // part of it filled: it rests on
    } kind;
    std::uint64_t id;  // the request's ID (accepted, replaced, rejected); the order's (cancelled, filled, partly_filled)
    std::uint32_t price = 0;     // filled, partly_filled: the fill's (four implied decimals)
    std::uint32_t quantity = 0;  // filled, partly_filled: the fill's shares (filled: the last fill)
    bool operator==(const order_answer&) const = default;
};

struct strategy_config {
    std::uint64_t first_id = 1;  // the counter's first value
};

struct strategy_counts {
    std::uint64_t events = 0, answers = 0, unknown_answers = 0;  // unknown: no order or request has its ID
    std::uint64_t news = 0, replaces = 0, cancels = 0;
    bool operator==(const strategy_counts&) const = default;
};

template <class Out>
class strategy {
public:
    strategy(std::size_t instruments, Out& out, const strategy_config& c = {}) : out_(out), next_(c.first_id), s_(instruments) {}

    void on(const book_event& e) {
        ++counts_.events;
        state& s = s_[e.slot];
        s.locate = e.locate;
        s.lot = e.round_lot;
        const bool ok = usable(e);
        const bool new_bid = ok && (!s.was_usable || e.bid_price != s.last_bid);
        s.was_usable = ok;
        if (ok) s.last_bid = e.bid_price;
        s.sequence = e.sequence;
        if (s.pending.kind) {
            if (!ok) s.remembered = remember::cancel;
            else if (new_bid) s.remembered = remember::bid, s.remembered_bid = e.bid_price;
            return;
        }
        if (!ok) {
            if (s.order_id) send_cancel(e.slot, s);
        } else if (new_bid) {
            if (!s.order_id) send_new(e.slot, s, e.bid_price);
            else if (e.bid_price != s.order_price) send_replace(e.slot, s, e.bid_price);
        }
    }

    void answer(const order_answer& a) {
        ++counts_.answers;
        const std::uint16_t slot = slot_of(a.id);
        if (slot >= s_.size()) {
            ++counts_.unknown_answers;
            return;
        }
        state& s = s_[slot];
        const bool for_request = s.pending.kind && a.id == s.pending.id;
        const bool for_order = s.order_id && a.id == s.order_id;
        switch (a.kind) {
            case order_answer::accepted:
                if (!for_request || s.pending.kind != intent::new_order) return unknown();
                s.order_id = s.pending.id;
                s.order_price = s.pending.price;
                break;
            case order_answer::replaced:
                if (!for_request || s.pending.kind != intent::replace) return unknown();
                s.order_id = s.pending.id;
                s.order_price = s.pending.price;
                break;
            case order_answer::rejected:
                if (!for_request) return unknown();
                break;  // a new: still no order; a replace or cancel: the order as it was (or gone, if filled meanwhile)
            case order_answer::cancelled:
            case order_answer::filled:
                // By the order's ID, or by the request's (a replace done, then filled or cancelled by the venue)
                if (!for_order && !for_request) return unknown();
                s.order_id = 0;
                if (s.pending.kind && !for_request) return;  // a replace or cancel in flight is still answered (rejected): wait for it
                break;
            case order_answer::partly_filled:
                if (!for_order && !for_request) unknown();
                return;
        }
        s.pending = {};
        act(slot, s);
    }

    const strategy_counts& counts() const { return counts_; }
    // The instrument's order (0: none) and request in flight (kind 0: none).
    std::uint64_t order_of(std::size_t slot) const { return s_[slot].order_id; }
    std::uint32_t price_of(std::size_t slot) const { return s_[slot].order_price; }
    const intent& pending(std::size_t slot) const { return s_[slot].pending; }

private:
    enum class remember : std::uint8_t { nothing, bid, cancel };
    struct state {
        std::uint64_t order_id = 0;   // the resting order's current ID; 0: none
        std::uint32_t order_price = 0;
        intent pending{};             // kind 0: no request in flight
        remember remembered = remember::nothing;
        std::uint32_t remembered_bid = 0;
        std::uint32_t last_bid = 0;   // the last usable event's
        bool was_usable = false;
        std::uint16_t locate = 0;
        std::uint32_t lot = 0;
        std::uint64_t sequence = 0;
    };

    void unknown() { ++counts_.unknown_answers; }
    // The state is settled (no request in flight): what was remembered.
    void act(std::uint16_t slot, state& s) {
        const remember r = s.remembered;
        s.remembered = remember::nothing;
        s.sequence = 0;
        if (r == remember::cancel) {
            if (s.order_id) send_cancel(slot, s);
        } else if (r == remember::bid) {
            if (!s.order_id) send_new(slot, s, s.remembered_bid);
            else if (s.remembered_bid != s.order_price) send_replace(slot, s, s.remembered_bid);
        }
    }
    void send(state& s, intent::kind_t k, std::uint16_t slot, std::uint32_t price) {
        s.pending = {k, slot, s.locate, order_id(next_++, slot), k == intent::new_order ? 0 : s.order_id, price, s.lot, s.sequence};
        out_.intent(s.pending);
    }
    void send_new(std::uint16_t slot, state& s, std::uint32_t bid) {
        ++counts_.news;
        send(s, intent::new_order, slot, bid);
    }
    void send_replace(std::uint16_t slot, state& s, std::uint32_t bid) {
        ++counts_.replaces;
        send(s, intent::replace, slot, bid);
    }
    void send_cancel(std::uint16_t slot, state& s) {
        ++counts_.cancels;
        send(s, intent::cancel, slot, s.order_price);
    }

    Out& out_;
    std::uint64_t next_;
    std::vector<state> s_;
    strategy_counts counts_;
};

}  // namespace ttt
