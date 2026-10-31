// The publisher's updates as book events (note 002, D3 A, for note 001's
// arrangement C): a consumer process of the feed handler's publisher (its note
// 008) gives each update it reads to `on`, and the adapter keeps each traded
// instrument's state from them, sending a book event to `Out` when it
// changes, the same events the sink gives in the handler.
//   level        best bid and ask after the change, and the reliability
//   status       the trading state (from a trading-state update), and the reliability
//   instrument, imbalance
//                the reliability
//   feed         the feed's state, for every instrument
//   directory    the instrument's locate (by symbol) and round lot
//   reset        a join: the books follow, then the consumer is live; until
//                joined() nothing is sent, then what changed, the trading
//                states read from the state table (the publisher does not
//                replay status updates)
// The locates come only from directory updates, never replayed: the consumer
// must attach before the day's Stock Directory messages (the program's
// --wait-consumers).
// Design: docs/design/002-book-events-strategy.md
#pragma once

#include <ttt/events.hpp>

#include <itchfh/publication.hpp>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>

namespace ttt {

template <class Out>
class adapter {
public:
    adapter(instruments& i, Out& out) : ins_(i), out_(out), state_(i.size()), last_(i.size()) {}

    void on(const itchfh::pub::update& u) {
        namespace pub = itchfh::pub;
        switch (u.k) {
            case pub::kind::directory: {
                itchfh::symbol sym;
                std::memcpy(sym.data(), u.raw, sym.size());
                const std::uint16_t s = ins_.directory(u.locate, sym);
                if (s == no_slot) return;
                std::uint32_t lot;
                std::memcpy(&lot, u.raw + 10, sizeof(lot));  // after the symbol, the market category and financial status
                state_[s].round_lot = lot;
                set(s, u);
                return;
            }
            case pub::kind::level: {
                const std::uint16_t s = ins_.slot(u.locate);
                if (s == no_slot) return;
                book_event& e = state_[s];
                e.bid_price = u.level.bid_shares ? u.level.bid_price : 0;
                e.ask_price = u.level.ask_shares ? u.level.ask_price : 0;
                e.bid_shares = u.level.bid_shares;
                e.ask_shares = u.level.ask_shares;
                set(s, u);
                return;
            }
            case pub::kind::status: {
                const std::uint16_t s = ins_.slot(u.locate);
                if (s == no_slot) return;
                if (u.field == itchfh::status_field::trading_state) state_[s].trading_state = static_cast<char>(u.raw[0]);
                set(s, u);
                return;
            }
            case pub::kind::instrument:
            case pub::kind::imbalance: {
                const std::uint16_t s = ins_.slot(u.locate);
                if (s != no_slot) set(s, u);
                return;
            }
            case pub::kind::feed:
                for (std::size_t s = 0; s < ins_.size(); ++s) {
                    if (!ins_.known(s)) continue;
                    book_event& e = state_[s];
                    e.reliability = static_cast<std::uint8_t>((e.reliability & ~pub::feed_mask) | static_cast<std::uint8_t>(u.feed.state));
                    e.sequence = 0;
                    e.timestamp_ns = u.timestamp_ns;
                    send(static_cast<std::uint16_t>(s));
                }
                return;
            case pub::kind::reset:
                joining_ = true;
                for (book_event& e : state_) e.bid_price = e.ask_price = 0, e.bid_shares = e.ask_shares = 0;
                return;
            default:
                return;
        }
    }

    // Note 005: each event stamped when made (t1), for the measurement only.
    void stamp(bool on) { stamping_ = on; }

    // The consumer is live after a join: `read(locate, entry_view&)` gives the state table's entry.
    template <class Read>
    void joined(Read&& read) {
        joining_ = false;
        for (std::size_t s = 0; s < ins_.size(); ++s) {
            if (!ins_.known(s)) continue;
            itchfh::pub::entry_view v;
            if (read(ins_.locate(s), v)) state_[s].trading_state = v.trading_state;
            send(static_cast<std::uint16_t>(s));
        }
    }

private:
    void set(std::uint16_t s, const itchfh::pub::update& u) {
        book_event& e = state_[s];
        e.reliability = u.reliability;
        e.sequence = u.sequence;
        e.timestamp_ns = u.timestamp_ns;
        e.locate = u.locate;
        e.slot = s;
        send(s);
    }
    void send(std::uint16_t s) {
        if (joining_ || state_[s].same_state(last_[s])) return;
        last_[s] = state_[s];
        if (stamping_) {
            timespec t;
            ::clock_gettime(CLOCK_MONOTONIC, &t);
            state_[s].made_ns = t.tv_sec * 1'000'000'000LL + t.tv_nsec;
        }
        out_.push(state_[s]);
    }

    instruments& ins_;
    Out& out_;
    bool stamping_ = false;
    std::vector<book_event> state_, last_;
    bool joining_ = false;
};

}  // namespace ttt
