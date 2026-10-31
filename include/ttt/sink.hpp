// The strategy's sink in the feed handler (note 002, D3 A): given to its
// handler in place of the publisher, on the feed thread. For a traded
// instrument, every call that can change its state (an order message applied
// or refused, a status field, an imbalance, its directory entry, the feed's
// state) reads the state back from the book and the handler, and if it
// differs from the last one sent, sends a book event to `Out` (the ring of D4,
// or a test's). Every other locate costs one table lookup.
//   strategy_feed<Book, Out>: the handler with this sink, as feed_one's
//   handler: on(sequence, message), feed(state, next), tick(now).
// The calls rebind(), watch(), mute() and restored() are the publisher's, for
// a recovery from a snapshot (the feed handler's note 011).
// Design: docs/design/002-book-events-strategy.md
#pragma once

#include <ttt/events.hpp>

#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/market_status.hpp>
#include <itchfh/publication.hpp>

#include <cstdint>

namespace ttt {

template <class Book, class Out>
class sink {
public:
    sink(const Book& b, instruments& i, Out& out) : book_(&b), ins_(i), out_(out), last_(i.size()), round_lot_(i.size(), 0) {}
    // No book yet: the feed handler's recovery rebinds it to its own before the first message (note 004).
    sink(instruments& i, Out& out) : book_(nullptr), ins_(i), out_(out), last_(i.size()), round_lot_(i.size(), 0) {}

    void rebind(const Book& b) { book_ = &b; }
    template <class H>
    void watch(const H& h) {
        status_ = &h.statuses();
    }
    void mute(bool on) { muted_ = on; }
    void restored() {
        muted_ = false;
        all();
    }

    // ---- from the feed
    void begin(std::uint64_t sequence, std::uint64_t timestamp_ns) {
        seq_ = sequence;
        ts_ = timestamp_ns;
    }
    void feed(itchfh::feed_state s, std::uint64_t) {
        feed_ = s;
        seq_ = 0;  // a change of the feed's state, not of a message
        all();
    }
    void tick(std::int64_t) {
        if constexpr (requires { out_.flush(); }) out_.flush();
    }

    // ---- the handler's sink (the feed handler's note 003)
    void book(const itchfh::applied& a) { changed(ins_.slot(a.locate)); }
    void status(std::uint16_t l, itchfh::status_field) { changed(ins_.slot(l)); }
    void imbalance(std::uint16_t l, const itchfh::imbalance&) { changed(ins_.slot(l)); }
    void trade(const itchfh::trade_report&) {}
    void trade_broken(const itchfh::trade_report&) {}
    void market(const itchfh::message&) {}
    void directory(std::uint16_t l, const itchfh::stock_directory& r) {
        const std::uint16_t s = ins_.directory(l, r.stock);
        if (s == no_slot) return;
        round_lot_[s] = r.round_lot_size;
        changed(s);
    }

    const book_event& last(std::size_t slot) const { return last_[slot]; }

private:
    void all() {
        for (std::size_t s = 0; s < ins_.size(); ++s)
            if (ins_.known(s)) changed(static_cast<std::uint16_t>(s));
    }
    void changed(std::uint16_t s) {
        if (s == no_slot || muted_) return;
        const std::uint16_t l = ins_.locate(s);
        book_event e;
        const auto bb = book_->best(l, itchfh::side::bid), ba = book_->best(l, itchfh::side::ask);
        if (bb) e.bid_price = bb->price, e.bid_shares = bb->shares;
        if (ba) e.ask_price = ba->price, e.ask_shares = ba->shares;
        const itchfh::instrument_status* st = status_ ? &status_->of(l) : nullptr;
        e.reliability = itchfh::pub::reliability(feed_, book_->unreliable(l), st && st->not_since_snapshot != 0);
        e.trading_state = st ? st->trading_state : 0;
        e.round_lot = round_lot_[s];
        if (e.same_state(last_[s])) return;
        e.sequence = seq_;
        e.timestamp_ns = ts_;
        e.locate = l;
        e.slot = s;
        last_[s] = e;
        out_.push(e);
    }

    const Book* book_;
    instruments& ins_;
    Out& out_;
    const itchfh::market_status* status_ = nullptr;
    std::vector<book_event> last_;
    std::vector<std::uint32_t> round_lot_;
    itchfh::feed_state feed_ = itchfh::feed_state::waiting;
    std::uint64_t seq_ = 0, ts_ = 0;
    bool muted_ = false;
};

// The handler with the strategy's sink: what feed_one runs in arrangements A and B.
template <class Out, class Book = itchfh::default_book>
struct strategy_feed {
    sink<Book, Out> s;
    itchfh::handler<Book, sink<Book, Out>> h;
    strategy_feed(Book& b, instruments& i, Out& out, const itchfh::handler_config& c = {}) : s(b, i, out), h(b, s, c) { s.watch(h); }
    void on(std::uint64_t sequence, const itchfh::message& m) {
        s.begin(sequence, m.timestamp_ns);
        h.on(m);
    }
    void feed(itchfh::feed_state st, std::uint64_t next) { s.feed(st, next); }
    void tick(std::int64_t now) { s.tick(now); }
};

}  // namespace ttt
