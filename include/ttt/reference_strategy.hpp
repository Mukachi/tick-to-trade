// The reference strategy (note 002, S2): note 002's table written a second
// time for clarity, over standard containers (a map by client order ID, a
// map by instrument), with no counter tricks: each request's instrument is
// looked up. The oracle the strategy is compared against over the sample
// day, given the same events and answers; same intents.
// Design: docs/design/002-book-events-strategy.md
#pragma once

#include <ttt/events.hpp>
#include <ttt/strategy.hpp>

#include <cstdint>
#include <map>
#include <optional>

namespace ttt {

template <class Out>
class reference_strategy {
public:
    reference_strategy(Out& out, const strategy_config& c = {}) : out_(out), next_(c.first_id) {}

    void on(const book_event& e) {
        instrument& in = by_slot_[e.slot];
        in.locate = e.locate;
        in.lot = e.round_lot;
        in.cause = e.sequence;
        const bool now_usable = usable(e);
        const bool bid_is_new = now_usable && (!in.last_usable || in.last_usable_bid != e.bid_price);
        if (now_usable) in.last_usable_bid = e.bid_price;
        in.last_usable = now_usable;

        if (in.request) {  // in flight: remember
            if (!now_usable) in.later = later_t{true, 0};
            else if (bid_is_new) in.later = later_t{false, e.bid_price};
            return;
        }
        if (!in.order) {
            if (bid_is_new) request(e.slot, in, intent::new_order, e.bid_price);
            return;
        }
        if (!now_usable) request(e.slot, in, intent::cancel, in.order->price);
        else if (bid_is_new && e.bid_price != in.order->price) request(e.slot, in, intent::replace, e.bid_price);
    }

    void answer(const order_answer& a) {
        const auto r = requests_.find(a.id);
        const auto o = orders_.find(a.id);
        const std::uint16_t slot = r != requests_.end() ? r->second : o != orders_.end() ? o->second : no_slot;
        if (slot == no_slot) return;
        instrument& in = by_slot_[slot];
        const bool is_request = in.request && in.request->id == a.id;
        const bool is_order = in.order && in.order->id == a.id;
        if (!is_request && !is_order) return;
        switch (a.kind) {
            case order_answer::accepted:
                if (!is_request || in.request->kind != intent::new_order) return;
                in.order = order_t{in.request->id, in.request->price};
                orders_[in.request->id] = slot;
                break;
            case order_answer::replaced:
                if (!is_request || in.request->kind != intent::replace) return;
                in.order = order_t{in.request->id, in.request->price};
                orders_[in.request->id] = slot;
                break;
            case order_answer::rejected:
                if (!is_request) return;
                break;
            case order_answer::cancelled:
            case order_answer::filled:
                in.order.reset();
                // A request in flight for the order is still answered: wait for it.
                if (in.request && !is_request) return;
                break;
            case order_answer::partly_filled:
                return;
        }
        in.request.reset();
        in.cause = 0;
        settle(slot, in);
    }

private:
    struct order_t {
        std::uint64_t id;
        std::uint32_t price;
    };
    struct later_t {
        bool cancel;
        std::uint32_t bid;
    };
    struct instrument {
        std::optional<order_t> order;
        std::optional<intent> request;
        std::optional<later_t> later;
        bool last_usable = false;
        std::uint32_t last_usable_bid = 0;
        std::uint16_t locate = 0;
        std::uint32_t lot = 0;
        std::uint64_t cause = 0;
    };

    void settle(std::uint16_t slot, instrument& in) {
        if (!in.later) return;
        const later_t l = *in.later;
        in.later.reset();
        if (l.cancel) {
            if (in.order) request(slot, in, intent::cancel, in.order->price);
        } else if (!in.order) {
            request(slot, in, intent::new_order, l.bid);
        } else if (in.order->price != l.bid) {
            request(slot, in, intent::replace, l.bid);
        }
    }
    void request(std::uint16_t slot, instrument& in, intent::kind_t k, std::uint32_t price) {
        const intent i{k, slot, in.locate, order_id(next_++, slot), in.order && k != intent::new_order ? in.order->id : 0, price, in.lot, in.cause};
        in.request = i;
        requests_[i.id] = slot;
        out_.intent(i);
    }

    Out& out_;
    std::uint64_t next_;
    std::map<std::uint16_t, instrument> by_slot_;
    std::map<std::uint64_t, std::uint16_t> requests_, orders_;
};

}  // namespace ttt
