// The pre-trade risk checks (note 003): the strategy's output (its intents)
// in, what passes to `Out` (the gateway's side: send(intent)), what is refused
// answered `rejected` (take_refusal(), for the loop to give the strategy, so
// the strategy is never called back inside its own call). The answers come
// through answer(), which moves the risk state and gives what the strategy is
// to be told.
//   Checks, in this order, for a new order or a replace; the first to fail
//   names the refusal: kill switch, session, permission, book usable,
//   duplicate, volume, value, collar, period collar, messages, instrument
//   exposure, firm exposure. A cancel passes every check (counted as a
//   message: Art. 15(1)(d)).
//   Counting at send (Art. 15(2)): a request passed is counted at once;
//   exposure (D5) is the instrument's position at cost, its open order's
//   price x shares left, and a request in flight: a new at its price x
//   quantity, a replace at its new price x the shares left, the old order
//   still counted (D3).
//   Windows (messages; fills, D4) on market time (D1): the ITCH timestamp of
//   the last book event; each keeps the times of its last N entries, so
//   "N within the window" is "the N-th most recent within it".
//   Kill switch (Art. 12; D4's throttle engages it): nothing but cancels
//   passes; every open order is cancelled by a cancel of the risk's own (its
//   ID with kill_bit set), and an order acknowledged or replaced while it is
//   engaged is cancelled then. Such a cancel's answer reaches the strategy as
//   the venue cancelling the order (the order's ID); its rejection (the order
//   filled or replaced meanwhile) does not reach it. Only reenable() ends it.
// Design: docs/design/003-pre-trade-risk.md
#pragma once

#include <ttt/events.hpp>
#include <ttt/limits.hpp>
#include <ttt/strategy.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace ttt {

enum class refusal : std::uint8_t {
    none, killed, session, permission, unusable, duplicate, volume, value, collar, period_collar, messages, instrument_exposure,
    firm_exposure
};
inline constexpr int refusal_kinds = 13;
inline const char* name(refusal r) {
    static const char* const n[] = {"none", "killed", "session", "permission", "unusable", "duplicate", "volume", "value",
                                    "collar", "period_collar", "messages", "instrument_exposure", "firm_exposure"};
    return n[static_cast<int>(r)];
}

inline constexpr std::uint64_t kill_bit = std::uint64_t{1} << 63;

struct risk_counts {
    std::uint64_t passed = 0, cancels = 0, fills = 0, kills = 0, kill_cancels = 0, unknown_answers = 0;
    std::array<std::uint64_t, refusal_kinds> refused{};
    bool operator==(const risk_counts&) const = default;
};

// The times of the last N entries.
class time_window {
public:
    time_window(std::uint32_t n, std::uint64_t ns) : t_(n ? n : 1, 0), ns_(ns) {}
    bool full(std::uint64_t now) const { return n_ == t_.size() && t_[next_] + ns_ > now; }
    void add(std::uint64_t now) {
        t_[next_] = now;
        next_ = (next_ + 1) % t_.size();
        if (n_ < t_.size()) ++n_;
    }
    void clear() { n_ = next_ = 0; }

private:
    std::vector<std::uint64_t> t_;
    std::uint64_t ns_;
    std::size_t next_ = 0, n_ = 0;
};

template <class Out>
class risk {
public:
    risk(const instruments& ins, const limits& l, Out& out)
        : ins_(ins), lim_(l), out_(out), s_(ins.size()), messages_(l.max_messages, l.messages_window_ns), fills_(l.max_fills, l.fills_window_ns) {
        for (std::size_t i = 0; i < ins.size(); ++i) {
            s_[i].permitted = l.permitted.empty();
            for (const std::string& p : l.permitted) s_[i].permitted |= to_symbol(p) == ins.symbol(i);
        }
    }

    // ---- inputs
    void on(const book_event& e) {
        if (e.timestamp_ns > now_) now_ = e.timestamp_ns;
        state& s = s_[e.slot];
        s.usable = usable(e);
        s.bid = e.bid_price;
        s.ask = e.ask_price;
        if (s.usable) {
            const std::uint64_t p = now_ / lim_.period_ns + 1;
            if (s.period != p) s.period = p, s.reference = e.bid_price;
        }
    }
    void session(bool up) { session_ = up; }
    void kill() {
        if (!killed_) ++counts_.kills;
        killed_ = true;
        for (std::size_t i = 0; i < s_.size(); ++i) cancel_for_kill(static_cast<std::uint16_t>(i));
    }
    void reenable() {
        killed_ = false;
        fills_.clear();
    }

    // ---- the strategy's output
    void intent(const ttt::intent& i) {
        state& s = s_[i.slot];
        if (i.kind == intent::cancel) {
            ++counts_.cancels;
            messages_.add(now_);
            s.cancel = i.id;
            out_.send(i);
            return;
        }
        const refusal r = check(i);
        if (r != refusal::none) {
            ++counts_.refused[static_cast<int>(r)];
            refusals_[(r_head_ + r_count_++) % refusals_.size()] = order_answer{order_answer::rejected, i.id};
            return;
        }
        ++counts_.passed;
        messages_.add(now_);
        s.req = i;
        recount(s);
        out_.send(i);
    }
    // A refused intent's answer, for the strategy.
    bool take_refusal(order_answer& a) {
        if (r_count_ == 0) return false;
        a = refusals_[r_head_];
        r_head_ = (r_head_ + 1) % refusals_.size();
        --r_count_;
        return true;
    }

    // ---- the gateway's answers: true, with `to`, if the strategy is to be told
    bool answer(const order_answer& a, order_answer& to) {
        const std::uint16_t slot = slot_of(a.id);
        to = a;
        if (slot >= s_.size()) {
            ++counts_.unknown_answers;
            return true;
        }
        state& s = s_[slot];
        if (s.kill_cancel && a.id == s.kill_cancel) {
            s.kill_cancel = 0;
            if (a.kind == order_answer::cancelled && s.open.id) {
                to = order_answer{order_answer::cancelled, s.open.id};
                s.open = {};
                recount(s);
                return true;
            }
            cancel_for_kill(slot);  // rejected: the order replaced meanwhile is cancelled again; filled, nothing left
            return false;
        }
        bool for_req = s.req.kind && a.id == s.req.id, for_open = s.open.id && a.id == s.open.id;
        if (for_req && s.req.kind == intent::replace && (a.kind == order_answer::filled || a.kind == order_answer::partly_filled)) {
            // A fill under the replace's ID before its answer: the replace was done.
            replaced(s);
            for_req = false;
            for_open = true;
        }
        switch (a.kind) {
            case order_answer::accepted:
                if (!for_req || s.req.kind != intent::new_order) break;
                s.open = {s.req.id, s.req.price, s.req.quantity};
                s.req = {};
                cancel_for_kill(slot);
                break;
            case order_answer::replaced:
                if (!for_req || s.req.kind != intent::replace) break;
                replaced(s);
                cancel_for_kill(slot);
                break;
            case order_answer::rejected:
                if (for_req) s.req = {};
                else if (s.cancel && a.id == s.cancel) s.cancel = 0;
                break;
            case order_answer::cancelled:
                if (s.cancel && a.id == s.cancel) s.cancel = 0, s.open = {};
                else if (for_open || for_req) {
                    s.open = {};
                    if (for_req) s.req = {};
                }
                break;
            case order_answer::partly_filled:
            case order_answer::filled:
                if (!for_open && !for_req) break;
                fill(s, a, for_open);
                if (a.kind == order_answer::filled) {
                    if (for_open) s.open = {};
                    else s.req = {};  // a new order filled before its acknowledgement
                }
                recount(s);
                if (!killed_ && fills_.full(now_)) kill();  // D4: Art. 15(3)'s throttle
                break;
        }
        recount(s);
        return true;
    }

    const risk_counts& counts() const { return counts_; }
    bool killed() const { return killed_; }
    std::uint64_t exposure(std::size_t slot) const { return s_[slot].exposure; }
    std::uint64_t firm_exposure() const { return firm_; }
    std::uint64_t position(std::size_t slot) const { return s_[slot].shares; }
    std::uint64_t now() const { return now_; }

private:
    struct order_t {
        std::uint64_t id = 0;
        std::uint32_t price = 0, leaves = 0;
    };
    struct state {
        order_t open;
        ttt::intent req{};               // the strategy's new or replace in flight (kind 0: none)
        std::uint64_t cancel = 0;        // the strategy's cancel in flight
        std::uint64_t kill_cancel = 0;   // the risk's own cancel in flight
        std::uint64_t shares = 0, cost = 0;  // position, at cost
        std::uint64_t exposure = 0;
        bool usable = false, permitted = false;
        std::uint32_t bid = 0, ask = 0, reference = 0;
        std::uint64_t period = 0;  // the reference price's period + 1; 0: none yet
    };

    refusal check(const ttt::intent& i) const {
        const state& s = s_[i.slot];
        if (killed_) return refusal::killed;
        if (!session_) return refusal::session;
        if (!s.permitted) return refusal::permission;
        if (!s.usable) return refusal::unusable;
        if (s.req.kind || s.cancel || (i.kind == intent::new_order && s.open.id)) return refusal::duplicate;
        if (i.quantity > lim_.max_volume) return refusal::volume;
        const std::uint64_t p = i.price;
        if (p * i.quantity > lim_.max_value) return refusal::value;
        if ((s.ask && p * 10'000 > std::uint64_t{s.ask} * (10'000 + lim_.collar_bps)) ||
            (s.bid && p * 10'000 < std::uint64_t{s.bid} * (10'000 - (lim_.collar_bps < 10'000 ? lim_.collar_bps : 10'000))))
            return refusal::collar;
        if (s.reference) {
            const std::uint64_t r = s.reference, d = p > r ? p - r : r - p;
            if (d * 10'000 > r * lim_.period_collar_bps) return refusal::period_collar;
        }
        if (messages_.full(now_)) return refusal::messages;
        const std::uint64_t add = p * (i.kind == intent::new_order ? i.quantity : s.open.leaves);
        if (s.exposure + add > lim_.instrument_exposure) return refusal::instrument_exposure;
        if (firm_ + add > lim_.firm_exposure) return refusal::firm_exposure;
        return refusal::none;
    }
    // The replace in flight done: the order takes its ID and price. If the order
    // was gone by our account, the venue says it lives: counted at the replace's
    // whole quantity.
    void replaced(state& s) {
        if (!s.open.id) s.open.leaves = s.req.quantity;
        s.open.id = s.req.id;
        s.open.price = s.req.price;
        s.req = {};
    }
    void recount(state& s) {
        std::uint64_t e = s.cost + std::uint64_t{s.open.price} * s.open.leaves;
        if (s.req.kind == intent::new_order) e += std::uint64_t{s.req.price} * s.req.quantity;
        else if (s.req.kind == intent::replace) e += std::uint64_t{s.req.price} * s.open.leaves;
        firm_ = firm_ - s.exposure + e;
        s.exposure = e;
    }
    void fill(state& s, const order_answer& a, bool for_open) {
        ++counts_.fills;
        s.shares += a.quantity;
        s.cost += std::uint64_t{a.price} * a.quantity;
        std::uint32_t& left = for_open ? s.open.leaves : s.req.quantity;
        left -= a.quantity < left ? a.quantity : left;
        fills_.add(now_);
    }
    void cancel_for_kill(std::uint16_t slot) {
        state& s = s_[slot];
        if (!killed_ || !s.open.id || s.kill_cancel) return;
        s.kill_cancel = kill_bit | next_kill_++ << 16 | slot;
        ++counts_.kill_cancels;
        messages_.add(now_);
        out_.send(ttt::intent{intent::cancel, slot, ins_.locate(slot), s.kill_cancel, s.open.id, s.open.price, s.open.leaves, 0});
    }

    const instruments& ins_;
    limits lim_;
    Out& out_;
    std::vector<state> s_;
    time_window messages_, fills_;
    std::uint64_t now_ = 0, firm_ = 0, next_kill_ = 1;
    bool session_ = false, killed_ = false;
    risk_counts counts_;
    std::array<order_answer, 2 * max_instruments> refusals_{};
    std::size_t r_head_ = 0, r_count_ = 0;
};

}  // namespace ttt
