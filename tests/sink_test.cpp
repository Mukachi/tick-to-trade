// The sink and the adapter (note 002, S1 in small): the feed handler's
// synthetic stream, with halts and resumptions, an event the book refuses and
// the feed gapped and live again added, into the handler on the fast book,
// whose sink is a tee to the strategy's sink and the publisher; a consumer in
// this process reads the publisher's updates into the adapter. After every
// message and every change of the feed's state, the book events each sent
// equal those derived from the reference handler and reference book (best
// bid and ask, trading state, reliability, round lot, sequence).
// Spec: docs/design/002-book-events-strategy.md
#include "check.hpp"

#include <ttt/adapter.hpp>
#include <ttt/sink.hpp>

#include <itchfh/consumer.hpp>
#include <itchfh/publisher.hpp>
#include <itchfh/reference_book.hpp>
#include <itchfh/reference_handler.hpp>

#include <synthetic_stream.hpp>  // the feed handler's tests/

#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace ttt;
using ttt::test::check;
using itchfh::default_book;

itchfh::book_config small_book() {
    itchfh::book_config c;
    c.orders = 200'000;
    c.levels = 50'000;
    c.ref_slots = 1 << 17;
    c.ref_span = 1 << 20;
    c.level_arena = 1 << 18;
    c.prefault = false;
    return c;
}
itchfh::handler_config small_handler() {
    itchfh::handler_config c;
    c.matches = 100'000;
    c.participants = 10'000;
    c.prefault = false;
    return c;
}

struct collect {
    std::vector<book_event> v;
    void push(const book_event& e) { v.push_back(e); }
};

// Every handler call to both the strategy's sink and the publisher.
struct both {
    sink<default_book, collect>& a;
    itchfh::publisher<default_book>& b;
    void book(const itchfh::applied& x) { a.book(x), b.book(x); }
    void trade(const itchfh::trade_report& x) { a.trade(x), b.trade(x); }
    void trade_broken(const itchfh::trade_report& x) { a.trade_broken(x), b.trade_broken(x); }
    void status(std::uint16_t l, itchfh::status_field f) { a.status(l, f), b.status(l, f); }
    void imbalance(std::uint16_t l, const itchfh::imbalance& x) { a.imbalance(l, x), b.imbalance(l, x); }
    void market(const itchfh::message& m) { a.market(m), b.market(m); }
    void directory(std::uint16_t l, const itchfh::stock_directory& r) { a.directory(l, r), b.directory(l, r); }
};

struct no_sink {
    void book(const itchfh::applied&) {}
    void trade(const itchfh::trade_report&) {}
    void trade_broken(const itchfh::trade_report&) {}
    void status(std::uint16_t, itchfh::status_field) {}
    void imbalance(std::uint16_t, const itchfh::imbalance&) {}
    void market(const itchfh::message&) {}
    void directory(std::uint16_t, const itchfh::stock_directory&) {}
};

// The events S1 expects, derived from the reference book and handler after
// every message by reading each traded instrument's state.
struct derived {
    instruments& ins;
    const itchfh::reference_book& book;
    const itchfh::reference_handler<itchfh::reference_book, no_sink>& h;
    std::vector<std::uint32_t> lots;
    std::vector<book_event> last;
    itchfh::feed_state feed = itchfh::feed_state::waiting;
    collect out;
    void after(std::uint64_t seq, std::uint64_t ts) {
        for (std::size_t s = 0; s < ins.size(); ++s) {
            if (!ins.known(s)) continue;
            const std::uint16_t l = ins.locate(s);
            book_event e;
            const auto bb = book.best(l, itchfh::side::bid), ba = book.best(l, itchfh::side::ask);
            if (bb) e.bid_price = bb->price, e.bid_shares = bb->shares;
            if (ba) e.ask_price = ba->price, e.ask_shares = ba->shares;
            const itchfh::instrument_status st = h.status_of(l);
            e.reliability = itchfh::pub::reliability(feed, book.unreliable(l), st.not_since_snapshot != 0);
            e.trading_state = st.trading_state;
            e.round_lot = lots[s];
            if (e.same_state(last[s])) continue;
            e.sequence = seq;
            e.timestamp_ns = ts;
            e.locate = l;
            e.slot = static_cast<std::uint16_t>(s);
            last[s] = e;
            out.push(e);
        }
    }
};

std::string first_difference(const std::vector<book_event>& a, const std::vector<book_event>& b) {
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (!(a[i] == b[i]))
            return "event " + std::to_string(i) + " (sequence " + std::to_string(a[i].sequence) + " vs " + std::to_string(b[i].sequence) + ")";
    return a.size() == b.size() ? "" : "counts " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
}

}  // namespace

int main() {
    const std::vector<std::string_view> traded = {"S1", "S2", "S4", "S7", "ZZZZ"};
    instruments ins_sink(traded), ins_adapter(traded), ins_ref(traded);

    auto book = std::make_unique<default_book>(small_book());
    collect from_sink, from_adapter;
    sink<default_book, collect> sk(*book, ins_sink, from_sink);
    auto pub = std::make_unique<itchfh::publisher<default_book>>(*book);
    both t{sk, *pub};
    auto h = std::make_unique<itchfh::handler<default_book, both>>(*book, t, small_handler());
    sk.watch(*h);
    pub->watch(*h);

    auto ref_book = std::make_unique<itchfh::reference_book>();
    no_sink ns;
    auto ref = std::make_unique<itchfh::reference_handler<itchfh::reference_book, no_sink>>(*ref_book, ns);
    derived d{ins_ref, *ref_book, *ref, std::vector<std::uint32_t>(max_instruments, 0), std::vector<book_event>(max_instruments), {}, {}};

    const std::string name = "/ttt-st-" + std::to_string(::getpid());
    std::string why;
    check(pub->open(name, why), "the publisher's segments created " + why);
    itchfh::pub::consumer con;
    check(con.attach(name, why), "a consumer attached " + why);
    adapter<collect> ad(ins_adapter, from_adapter);
    const auto drain = [&] {
        itchfh::pub::update u;
        while (con.poll(u)) ad.on(u);
    };
    for (int i = 0; i < 10 && con.state() != itchfh::pub::slot_live; ++i) {
        pub->tick(0);
        drain();
    }
    check(con.state() == itchfh::pub::slot_live, "the consumer joined before the day (the program's --wait-consumers)");
    ad.joined([&](std::uint16_t l, itchfh::pub::entry_view& v) { return con.read(l, v); });

    // The derived events of a feed change carry the last message's timestamp, as the sink's do.
    std::uint64_t last_ts = 0;
    const auto feed_change = [&](itchfh::feed_state s, std::uint64_t next) {
        sk.feed(s, next);
        pub->feed(s, next);
        d.feed = s;
        d.after(0, last_ts);
        drain();
    };
    feed_change(itchfh::feed_state::live, 1);

    std::vector<itchfh::message> day = itchfh::test::stream(200'000);
    // Halts and resumptions of S2, and a delete of no live order for S4 (the book refuses it: unsound)
    for (std::size_t i = 20'000; i < day.size(); i += 20'000) {
        itchfh::message hm{};
        hm.type = 'H';
        hm.locate = 2;
        hm.timestamp_ns = day[i].timestamp_ns;
        hm.H = {{'S', '2', ' ', ' ', ' ', ' ', ' ', ' '}, (i / 20'000) % 2 ? 'H' : 'T', ' ', {' ', ' ', ' ', ' '}};
        day.insert(day.begin() + static_cast<std::ptrdiff_t>(i), hm);
    }
    itchfh::message bad{};
    bad.type = 'D';
    bad.locate = 4;
    bad.timestamp_ns = day[150'000].timestamp_ns;
    bad.D.ref = 999'999'999;
    day.insert(day.begin() + 150'000, bad);

    std::uint64_t seq = 0;
    for (std::size_t i = 0; i < day.size(); ++i) {
        const itchfh::message& m = day[i];
        ++seq;
        if (m.type == 'R') {
            const std::uint16_t s = ins_ref.directory(m.locate, m.R.stock);
            if (s != no_slot) d.lots[s] = m.R.round_lot_size;
        }
        sk.begin(seq, m.timestamp_ns);
        pub->begin(seq, m.timestamp_ns);
        h->on(m);
        ref->on(m);
        last_ts = m.timestamp_ns;
        d.after(seq, m.timestamp_ns);
        drain();
        if (i == 60'000) feed_change(itchfh::feed_state::gapped, seq + 1);
        if (i == 60'500) feed_change(itchfh::feed_state::live, seq + 1);
    }

    std::size_t halts = 0, unsound = 0, gapped = 0;
    for (const book_event& e : d.out.v) {
        halts += e.trading_state == 'H';
        unsound += (e.reliability & itchfh::pub::book_unsound) != 0;
        gapped += itchfh::pub::feed_of(e.reliability) == itchfh::feed_state::gapped;
    }
    check(halts > 0 && unsound > 0 && gapped > 0, "the stream covers a halt, an unsound book and a gapped feed (" + std::to_string(halts) + ", " +
                                                      std::to_string(unsound) + ", " + std::to_string(gapped) + " events)");
    const std::string ds = first_difference(from_sink.v, d.out.v), da = first_difference(from_adapter.v, d.out.v);
    check(ds.empty(), "S1 in small: the sink's events equal those derived from the reference book (" + std::to_string(from_sink.v.size()) +
                          " events" + (ds.empty() ? "" : "; differ at " + ds) + ")");
    check(da.empty(), "S1 in small: the adapter's events, from the publisher's updates, equal them too" + (da.empty() ? "" : " (differ at " + da + ")"));
    bool only_traded = true;
    for (const book_event& e : from_sink.v) only_traded &= e.locate == 1 || e.locate == 2 || e.locate == 4 || e.locate == 7;
    check(only_traded && !ins_sink.known(4), "only the traded instruments' events; a symbol not in the directory never has one");
    return ttt::test::result();
}
