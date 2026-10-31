// The strategy thread's work in the program (note 004), the same in every
// arrangement: a book event to the risk checks and then the strategy (as
// risk_bench); each request the risk passes written as an order message into
// the gateway's application ring (D1) and its ID kept for S1; each slot back
// from the gateway read as an answer (D2), through the risk to the strategy;
// the gateway's session state to the risk (D3); the operator's commands from
// the block (D5); the counters to the block every 100 ms. The end of the day
// (D8): the kill switch engaged, then quiet() once every order is answered.
// No allocation and no system call on any of it (S4): the order ring full is
// waited on, never dropped (the risk has already counted the request).
// Note 005's stamps, if configured: each D and G passed listed with its
// message's sequence, t0 (the sender's intended time, read from its array now:
// it wraps), t1 (the event's) and t2 (now), in a list allocated at start.
// Design: docs/design/004-program.md
#pragma once

#include <ttt/block.hpp>
#include <ttt/orders.hpp>
#include <ttt/risk.hpp>
#include <ttt/strategy.hpp>

#include <fixgw/engine.hpp>

#include "stamps.hpp"  // the feed handler's bench/: the sender's intended send times

#include <lfipc/bench/clock.hpp>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <span>

namespace ttt {

template <class Engine>
class trader {
public:
    trader(const instruments& ins, const limits& l, Engine& gw, fixgw::app_ring& to, fixgw::app_ring& from, double ticks_per_ns,
           std::size_t passed_capacity, char arrangement)
        : gw_(gw), to_(to), from_(from), writer_(ins), utc_(ticks_per_ns), ro_{this}, so_{this}, risk_(ins, l, ro_),
          strat_(ins.size(), so_), slots_(ins.size()), per_ms_(ticks_per_ns * 1e6) {
        passed_ = std::make_unique<std::uint64_t[]>(passed_capacity);
        passed_capacity_ = passed_capacity;
        v_[mon::arrangement] = static_cast<std::uint64_t>(arrangement);
        last_refresh_ = lfipc::bench::stamp();
    }

    // A book event (the ring's, the sink's or the adapter's).
    void push(const book_event& e) {
        ++v_[mon::events];
        made_ns_ = e.made_ns;
        risk_.on(e);
        strat_.on(e);
        settle();
        made_ns_ = 0;
    }

    // Note 005: one order traced.
    struct traced {
        std::uint64_t id, sequence;  // sequence 0: caused by an answer
        std::int64_t t0, t1, t2;
        char type;
    };
    void trace(const itchfh::bench::stamp_array* stamps, std::size_t capacity) {
        stamps_ = stamps;
        traced_ = std::make_unique<traced[]>(capacity);
        traced_capacity_ = capacity;
    }
    std::span<const traced> traces() const { return {traced_.get(), traced_n_}; }
    std::uint64_t traces_lost() const { return traces_lost_; }
    void flush() {}

    // Each pass of the thread: the gateway's answers, its session's state,
    // the operator's command, the counters when due. False once told to stop.
    bool pass(mon::block* b) {
        fixgw::app_slot s;
        while (from_.try_pop(s)) {
            order_answer a;
            const answer_read r = read_answer(s, a);
            if (s.kind == fixgw::app_slot::refused) {
                ++v_[mon::gateway_refused];
                const fixgw::order_event e = fixgw::slot_event(s);
                why_length_ = e.reason_length;
                std::memcpy(why_, e.reason, sizeof(why_));
            }
            if (r == answer_read::unmapped) ++v_[mon::unmapped];
            if (r != answer_read::answer) continue;
            ++v_[mon::answers];
            order_answer to;
            if (risk_.answer(a, to)) strat_.answer(to);
            settle();
        }
        const bool up = gw_.logged_on();
        if (up != logged_on_) {
            logged_on_ = up;
            risk_.session(up);
        }
        if (b) {
            const std::uint64_t w = b->command_word();
            if ((w >> 8) != taken_) {
                taken_ = w >> 8;
                command(static_cast<std::uint8_t>(w & 0xff));
            }
            const lfipc::bench::ticks now = lfipc::bench::stamp();
            if (static_cast<double>(now - last_refresh_) >= 100 * per_ms_) {
                refresh(*b);
                last_refresh_ = now;
            }
        }
        return !stopping_;
    }

    // D8: the end of the day (or `stop`): every open order cancelled.
    void end() {
        v_[mon::ending] = 1;
        if (!risk_.killed()) risk_.kill();
        settle();
    }
    void feed_ended() { v_[mon::feed_ended] = 1; }
    // The feed is being read (its books built, its socket open; C: the consumer joined).
    void feed_ready() { v_[mon::feed_ready] = 1; }
    // Every order of ours answered as gone, and no request in flight.
    bool quiet() const {
        for (std::size_t i = 0; i < v_slots(); ++i)
            if (strat_.order_of(i) || strat_.pending(i).kind) return false;
        return true;
    }

    void refresh(mon::block& b) {
        const strategy_counts& sc = strat_.counts();
        const risk_counts& rc = risk_.counts();
        v_[mon::logged_on] = logged_on_;
        v_[mon::killed] = risk_.killed();
        v_[mon::unknown_strategy] = sc.unknown_answers;
        v_[mon::news] = sc.news;
        v_[mon::replaces] = sc.replaces;
        v_[mon::cancels] = sc.cancels;
        v_[mon::passed] = rc.passed;
        v_[mon::risk_cancels] = rc.cancels;
        v_[mon::fills] = rc.fills;
        v_[mon::kills] = rc.kills;
        v_[mon::kill_cancels] = rc.kill_cancels;
        v_[mon::unknown_risk] = rc.unknown_answers;
        v_[mon::firm_exposure] = risk_.firm_exposure();
        std::uint64_t open = 0, refused = 0;
        for (std::size_t i = 0; i < v_slots(); ++i) open += strat_.order_of(i) != 0;
        v_[mon::open_orders] = open;
        for (int k = 1; k < refusal_kinds; ++k) refused += rc.refused[k], v_[mon::refused_first + k - 1] = rc.refused[k];
        v_[mon::refused_total] = refused;
        v_[mon::command_taken] = taken_;
        ++v_[mon::refreshes];
        b.write(v_);
    }
    void ring_counts(std::uint64_t set_aside, std::uint64_t replaced) {
        v_[mon::ring_set_aside] = set_aside;
        v_[mon::ring_replaced] = replaced;
    }

    std::span<const std::uint64_t> passed() const { return {passed_.get(), passed_n_}; }
    std::uint64_t passed_lost() const { return passed_lost_; }  // past the list's capacity (S1 then cannot be checked)
    const risk_counts& risk_counters() const { return risk_.counts(); }
    const strategy_counts& strategy_counters() const { return strat_.counts(); }
    const mon::values& counters() const { return v_; }
    // The gateway's reason for the last request it refused.
    std::string_view last_gateway_refusal() const { return {why_, why_length_}; }
    bool logged_on() const { return logged_on_; }
    bool killed() const { return risk_.killed(); }

private:
    struct risk_out {
        trader* t;
        void send(const intent& i) { t->send(i); }
    };
    struct strategy_out {
        trader* t;
        void intent(const ttt::intent& i) { t->risk_.intent(i); }
    };

    std::size_t v_slots() const { return slots_; }
    void send(const intent& i) {
        order_message m;
        writer_.write(i, utc_.now_ns(), m);
        // As fixgw::submit_order, without its doorbell: the engine busy-polls, so
        // no write(2) is on the path, even one never taken (S4's check reads the code).
        fixgw::app_slot s;
        s.kind = fixgw::app_slot::order;
        s.type_length = 1;
        s.length = m.length;
        s.type[0] = m.type[0];
        std::memcpy(s.data, m.body, m.length);
        while (!to_.try_push(s)) ++v_[mon::app_ring_full];
        if (stamps_ && i.kind != intent::cancel) {
            if (traced_n_ < traced_capacity_) {
                timespec t;
                ::clock_gettime(CLOCK_MONOTONIC, &t);
                traced_[traced_n_++] = {i.id, i.sequence, i.sequence ? stamps_->get(i.sequence) : 0, i.sequence ? made_ns_ : 0,
                                        t.tv_sec * 1'000'000'000LL + t.tv_nsec, m.type[0]};
            } else {
                ++traces_lost_;
            }
        }
        if (passed_n_ < passed_capacity_) passed_[passed_n_++] = i.id;
        else ++passed_lost_;
    }
    void settle() {
        order_answer a;
        while (risk_.take_refusal(a)) strat_.answer(a);
    }
    void command(std::uint8_t c) {
        v_[mon::last_command] = c;
        if (c == mon::kill) risk_.kill();
        else if (c == mon::reenable) risk_.reenable();
        else if (c == mon::stop) stopping_ = true;
        settle();
    }

    Engine& gw_;
    fixgw::app_ring& to_;
    fixgw::app_ring& from_;
    order_writer writer_;
    utc_clock utc_;
    risk_out ro_;
    strategy_out so_;
    risk<risk_out> risk_;
    strategy<strategy_out> strat_;
    std::size_t slots_;
    double per_ms_;
    lfipc::bench::ticks last_refresh_;
    std::unique_ptr<std::uint64_t[]> passed_;  // allocated at start: no growth on the path
    std::size_t passed_n_ = 0, passed_capacity_ = 0;
    std::uint64_t passed_lost_ = 0, taken_ = 0;
    bool logged_on_ = false, stopping_ = false;
    char why_[64] = {};
    std::int64_t made_ns_ = 0;
    const itchfh::bench::stamp_array* stamps_ = nullptr;
    std::unique_ptr<traced[]> traced_;
    std::size_t traced_n_ = 0, traced_capacity_ = 0;
    std::uint64_t traces_lost_ = 0;
    std::size_t why_length_ = 0;
    mon::values v_{};
};

}  // namespace ttt
