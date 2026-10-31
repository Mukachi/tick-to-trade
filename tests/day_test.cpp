// Note 002 over the sample day (S1, S2), label `data`: needs data/ (the feed
// handler's tools/fetch_day.py), so CI does not run it. Every message of the
// day, the 100 instruments of D5's list traded, into the handler on the
// default book whose sink is a tee to the strategy's sink and the publisher,
// a consumer in this process reading the publisher into the adapter; and
// into the reference handler on the reference book, from which the expected
// events are derived after every message (the instrument it names; every
// instrument after a change of the feed's state, here once at the start).
// After every message: the sink's and the adapter's new events equal the
// derived ones (S1). The strategy takes the sink's events, the reference
// strategy the derived ones, each answered by a model venue seeded alike;
// after every message their new intents are equal (S2).
//
// Usage: day_test [day.gz] [instruments.txt]
#include "check.hpp"
#include "venue_model.hpp"

#include "day_file.hpp"  // tools/

#include <ttt/adapter.hpp>
#include <ttt/reference_strategy.hpp>
#include <ttt/sink.hpp>
#include <ttt/strategy.hpp>

#include <itchfh/consumer.hpp>
#include <itchfh/publisher.hpp>
#include <itchfh/reference_book.hpp>
#include <itchfh/reference_handler.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace ttt;
using ttt::test::check;
using itchfh::default_book;

struct collect {
    std::vector<book_event> v;
    void push(const book_event& e) { v.push_back(e); }
};

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
using ref_handler = itchfh::reference_handler<itchfh::reference_book, no_sink>;

struct derived {
    instruments& ins;
    const itchfh::reference_book& book;
    const ref_handler& h;
    std::vector<std::uint32_t> lots;
    std::vector<book_event> last;
    itchfh::feed_state feed;
    collect out;
    void one(std::uint16_t s, std::uint64_t seq, std::uint64_t ts) {
        const std::uint16_t l = ins.locate(s);
        book_event e;
        const auto bb = book.best(l, itchfh::side::bid), ba = book.best(l, itchfh::side::ask);
        if (bb) e.bid_price = bb->price, e.bid_shares = bb->shares;
        if (ba) e.ask_price = ba->price, e.ask_shares = ba->shares;
        const itchfh::instrument_status st = h.status_of(l);
        e.reliability = itchfh::pub::reliability(feed, book.unreliable(l), st.not_since_snapshot != 0);
        e.trading_state = st.trading_state;
        e.round_lot = lots[s];
        if (e.same_state(last[s])) return;
        e.sequence = seq;
        e.timestamp_ns = ts;
        e.locate = l;
        e.slot = s;
        last[s] = e;
        out.push(e);
    }
    void all(std::uint64_t seq, std::uint64_t ts) {
        for (std::size_t s = 0; s < ins.size(); ++s)
            if (ins.known(s)) one(static_cast<std::uint16_t>(s), seq, ts);
    }
};

// A strategy with its own model venue: each event, then the venue's step.
template <template <class> class S>
struct trading {
    struct side_t {
        test::venue_out<S<side_t>>* out;
        void intent(const ttt::intent& i) { out->intent(i); }
    };
    test::venue<S<side_t>> venue;
    test::venue_out<S<side_t>> out{&venue, {}};
    side_t side{&out};
    std::unique_ptr<S<side_t>> s;
    explicit trading(std::size_t slots) : venue(slots, 7) {
        if constexpr (requires { S<side_t>(slots, side, strategy_config{}); }) s = std::make_unique<S<side_t>>(slots, side, strategy_config{1});
        else s = std::make_unique<S<side_t>>(side, strategy_config{1});
    }
    void on(const book_event& e) {
        s->on(e);
        venue.step(*s);
    }
};
template <class O>
using fast_strategy = strategy<O>;
template <class O>
using ref_strategy = reference_strategy<O>;

std::string first_difference(const std::vector<book_event>& a, const std::vector<book_event>& b) {
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (!(a[i] == b[i])) return "locate " + std::to_string(a[i].locate) + " vs " + std::to_string(b[i].locate);
    return a.size() == b.size() ? "" : "counts " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : TTT_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::string list = argc > 2 ? argv[2] : TTT_CONFIG_DIR "/open-100.txt";
    std::vector<std::string> names;
    {
        std::ifstream f(list);
        for (std::string s; std::getline(f, s);)
            if (!s.empty()) names.push_back(s);
    }
    const std::vector<std::string_view> traded(names.begin(), names.end());
    check(traded.size() == 100, list + ": " + std::to_string(traded.size()) + " instruments");
    day_file day(path);
    if (!day.open()) {
        check(false, path + ": cannot read (the feed handler's tools/fetch_day.py fetches it)");
        return ttt::test::result();
    }
    const auto t0 = std::chrono::steady_clock::now();
    instruments ins_sink(traded), ins_adapter(traded), ins_ref(traded);

    auto book = std::make_unique<default_book>();
    collect from_sink, from_adapter;
    sink<default_book, collect> sk(*book, ins_sink, from_sink);
    auto pub = std::make_unique<itchfh::publisher<default_book>>(*book);
    both t{sk, *pub};
    auto h = std::make_unique<itchfh::handler<default_book, both>>(*book, t);
    sk.watch(*h);
    pub->watch(*h);

    auto ref_book = std::make_unique<itchfh::reference_book>();
    no_sink ns;
    auto ref = std::make_unique<ref_handler>(*ref_book, ns);
    derived d{ins_ref, *ref_book, *ref, std::vector<std::uint32_t>(max_instruments, 0), std::vector<book_event>(max_instruments),
              itchfh::feed_state::waiting, {}};

    const std::string name = "/ttt-dt-" + std::to_string(::getpid());
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
    ad.joined([&](std::uint16_t l, itchfh::pub::entry_view& v) { return con.read(l, v); });

    trading<fast_strategy> fast(traded.size());
    trading<ref_strategy> slow(traded.size());

    d.feed = itchfh::feed_state::live;
    sk.feed(itchfh::feed_state::live, 1);
    pub->feed(itchfh::feed_state::live, 1);
    drain();
    d.all(0, 0);

    std::string problem;
    std::uint64_t n = 0, events = 0, intents = 0, compared = 0;
    itchfh::message m;
    const auto compare = [&](std::uint64_t seq) {
        if (!problem.empty()) return;
        if (auto p = first_difference(from_sink.v, d.out.v); !p.empty()) problem = "S1: message " + std::to_string(seq) + ": the sink's events differ: " + p;
        else if (auto q = first_difference(from_adapter.v, d.out.v); !q.empty()) problem = "S1: message " + std::to_string(seq) + ": the adapter's events differ: " + q;
        for (const book_event& e : from_sink.v) fast.on(e);
        for (const book_event& e : d.out.v) slow.on(e);
        events += d.out.v.size();
        from_sink.v.clear();
        from_adapter.v.clear();
        d.out.v.clear();
        if (problem.empty() && (fast.out.log.size() != slow.out.log.size() ||
                                !std::equal(fast.out.log.begin() + static_cast<std::ptrdiff_t>(compared), fast.out.log.end(),
                                            slow.out.log.begin() + static_cast<std::ptrdiff_t>(compared))))
            problem = "S2: message " + std::to_string(seq) + ": the intents differ";
        compared = fast.out.log.size();
    };
    while (day.next(m)) {
        ++n;
        const std::uint64_t seq = day.sequence();
        if (m.type == 'R') {
            const std::uint16_t s = ins_ref.directory(m.locate, m.R.stock);
            if (s != no_slot) d.lots[s] = m.R.round_lot_size;
        }
        sk.begin(seq, m.timestamp_ns);
        pub->begin(seq, m.timestamp_ns);
        h->on(m);
        ref->on(m);
        // The instrument the message names (an IPO Quoting Period Update names it by symbol)
        std::uint16_t s = ins_ref.slot(m.locate);
        if (m.type == 'K')
            for (std::size_t i = 0; i < ins_ref.size(); ++i)
                if (ins_ref.symbol(i) == m.K.stock && ins_ref.known(i)) s = static_cast<std::uint16_t>(i);
        if (s != no_slot) d.one(s, seq, m.timestamp_ns);
        drain();
        compare(seq);
        if (!problem.empty()) break;
    }
    intents = fast.out.log.size();
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(n > 400'000'000 && day.undecoded() == 0, std::to_string(n) + " messages decoded, none undecodable (" + std::to_string(static_cast<int>(secs)) + " s)");
    check(problem.empty(), "S1 and S2 over the day, 100 instruments: " + std::to_string(events) + " book events from the sink and the adapter equal those derived from the reference book after every message; " +
                               std::to_string(intents) + " intents of the strategy equal the reference strategy's" + (problem.empty() ? "" : " (" + problem + ")"));
    const auto& c = fast.s->counts();
    check(c.news > 0 && c.replaces > 0 && c.cancels > 0, "every kind of intent (" + std::to_string(c.news) + " new, " + std::to_string(c.replaces) +
                                                            " replace, " + std::to_string(c.cancels) + " cancel)");
    std::size_t known = 0;
    for (std::size_t i = 0; i < ins_sink.size(); ++i) known += ins_sink.known(i);
    check(known == traded.size(), "every listed instrument found in the day's directory (" + std::to_string(known) + ")");
    return ttt::test::result();
}
