// The risk checks' cost (note 003, S7), on CPU 2: the strategy's thread
// (note 001's D4 A) given the book events the strategy's sink gives for D5's
// 100 instruments over the first 50 million messages of the sample day (the
// open among them; collected once, first, into memory), the strategy's
// intents answered by note 002's model venue, in three cases:
//   no checks   the strategy's intents straight to the venue
//   checks      through the risk checks (config/limits.conf: the defaults)
//   reference   through the reference implementation
// Timed between start() and stop(), unsubtracted, each apart: a book event's
// handling (the checks' and the strategy's, and every intent it sends checked);
// an intent's check alone; an answer's handling (the checks' and then the
// strategy's, and what it sends). The venue's own work is not timed. The first
// 10,000 book events discarded. Run JSON through lockfree-ipc's harness.
// Design: docs/design/003-pre-trade-risk.md
//
// Usage: risk_bench [output.json] [day.gz]
#include "day_file.hpp"  // tools/
#include "venue_model.hpp"  // tests/

#include <ttt/limits.hpp>
#include <ttt/reference_risk.hpp>
#include <ttt/risk.hpp>
#include <ttt/sink.hpp>
#include <ttt/strategy.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace lfipc::bench;

constexpr int cpu = 2;
constexpr std::size_t prefix = 50'000'000, warmup = 10'000;
using hist = histogram<10, 40>;

struct collect {
    std::vector<ttt::book_event> v;
    void push(const ttt::book_event& e) { v.push_back(e); }
};

struct case_result {
    std::string name;
    hist event, check, answer;
    std::uint64_t intents = 0, sent = 0, refused = 0, answers = 0;
    double events_per_s = 0;
};

// No checks: the strategy's intents straight on.
template <class Out>
struct pass_through {
    Out& out;
    pass_through(const ttt::instruments&, const ttt::limits&, Out& o) : out(o) {}
    void on(const ttt::book_event&) {}
    void session(bool) {}
    void intent(const ttt::intent& i) { out.send(i); }
    bool take_refusal(ttt::order_answer&) { return false; }
    bool answer(const ttt::order_answer& a, ttt::order_answer& to) {
        to = a;
        return true;
    }
    ttt::risk_counts counts() const { return {}; }
};

template <template <class> class Checks>
struct runner {
    struct sent {
        runner* r;
        void send(const ttt::intent& i) {
            ++r->res->sent;
            r->to_venue.push_back(i);
        }
    };
    struct strategy_out {
        runner* r;
        void intent(const ttt::intent& i) {
            ++r->res->intents;
            const ticks a = start();
            r->checks->intent(i);
            const ticks b = stop();
            if (r->timing) r->res->check.record(b - a);
        }
    };
    struct router {
        runner* r;
        void answer(const ttt::order_answer& x) {
            ++r->res->answers;
            const ticks a = start();
            ttt::order_answer to;
            if (r->checks->answer(x, to)) r->strat->answer(to);
            r->settle();
            const ticks b = stop();
            if (r->timing) r->res->answer.record(b - a);
        }
    };

    case_result* res;
    sent so{this};
    strategy_out sto{this};
    router rt{this};
    std::unique_ptr<Checks<sent>> checks;
    std::unique_ptr<ttt::strategy<strategy_out>> strat;
    ttt::test::venue<router> venue;
    std::vector<ttt::intent> to_venue;
    bool timing = false;

    runner(case_result* r, const ttt::instruments& ins, const ttt::limits& l)
        : res(r), checks(std::make_unique<Checks<sent>>(ins, l, so)), strat(std::make_unique<ttt::strategy<strategy_out>>(ins.size(), sto)),
          venue(ins.size(), 7) {
        checks->session(true);
        to_venue.reserve(1024);
    }
    void settle() {
        ttt::order_answer a;
        while (checks->take_refusal(a)) strat->answer(a);
    }
    void run(const std::vector<ttt::book_event>& events) {
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < events.size(); ++k) {
            timing = k >= warmup;
            const ticks a = start();
            checks->on(events[k]);
            strat->on(events[k]);
            settle();
            const ticks b = stop();
            if (timing) res->event.record(b - a);
            for (const ttt::intent& i : to_venue) venue.intent(i);
            to_venue.clear();
            venue.step(rt);
            for (const ttt::intent& i : to_venue) venue.intent(i);
            to_venue.clear();
        }
        res->events_per_s = static_cast<double>(events.size()) / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const ttt::risk_counts c = checks->counts();
        for (const std::uint64_t n : c.refused) res->refused += n;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : TTT_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "risk");
    if (run.refused) return run.finish(false, "risk_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "risk_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "risk_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    std::vector<std::string> names;
    {
        std::ifstream f(TTT_CONFIG_DIR "/open-100.txt");
        for (std::string s; std::getline(f, s);)
            if (!s.empty()) names.push_back(s);
    }
    ttt::limits lim;
    std::string why;
    if (names.size() != 100 || !ttt::read_limits(TTT_CONFIG_DIR "/limits.conf", lim, why)) {
        std::fprintf(stderr, "risk_bench: config: %zu instruments %s\n", names.size(), why.c_str());
        return run.finish(false, "risk_bench");
    }
    const std::vector<std::string_view> traded(names.begin(), names.end());

    // The book events, once: the sink over the day's first messages.
    ttt::instruments ins(traded);
    collect events;
    {
        auto book = std::make_unique<itchfh::default_book>();
        auto feed = std::make_unique<ttt::strategy_feed<collect>>(*book, ins, events);
        feed->feed(itchfh::feed_state::live, 1);
        ttt::day_file day(path);
        itchfh::message m;
        std::size_t n = 0;
        while (day.open() && n < prefix && day.next(m)) feed->on(day.sequence(), m), ++n;
        if (n < prefix) {
            std::fprintf(stderr, "risk_bench: %s: %zu messages read\n", path.c_str(), n);
            return run.finish(false, "risk_bench");
        }
    }

    std::vector<std::unique_ptr<case_result>> results;
    const auto report = [&](case_result& r) {
        const auto ns = [&](const hist& h, double p) { const auto t = h.percentile(p); return t ? static_cast<double>(*t) / tpn : -1.0; };
        std::printf("%-10s %.2f M events/s; event p50 %.0f p99 %.0f ns; check p50 %.0f p99 %.0f ns; answer p50 %.0f p99 %.0f ns; %llu intents, %llu sent, %llu refused\n",
                    r.name.c_str(), r.events_per_s / 1e6, ns(r.event, 50), ns(r.event, 99), ns(r.check, 50), ns(r.check, 99), ns(r.answer, 50),
                    ns(r.answer, 99), static_cast<unsigned long long>(r.intents), static_cast<unsigned long long>(r.sent),
                    static_cast<unsigned long long>(r.refused));
        std::fflush(stdout);
    };
    {
        auto r = std::make_unique<case_result>();
        r->name = "no checks";
        auto x = std::make_unique<runner<pass_through>>(r.get(), ins, lim);
        x->run(events.v);
        report(*r);
        results.push_back(std::move(r));
    }
    {
        auto r = std::make_unique<case_result>();
        r->name = "checks";
        auto x = std::make_unique<runner<ttt::risk>>(r.get(), ins, lim);
        x->run(events.v);
        report(*r);
        results.push_back(std::move(r));
    }
    {
        auto r = std::make_unique<case_result>();
        r->name = "reference";
        auto x = std::make_unique<runner<ttt::reference_risk>>(r.get(), ins, lim);
        x->run(events.v);
        report(*r);
        results.push_back(std::move(r));
    }
    const bool same = results[1]->sent == results[2]->sent && results[1]->intents == results[2]->intents && results[1]->refused == results[2]->refused;

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto stats = [&](const char* key, const hist& h) {
        const auto ns = [&](double p) { const auto t = h.percentile(p); return t ? static_cast<double>(*t) / tpn : -1.0; };
        run.w.key(key);
        run.w.begin_object();
        run.w.member("count", h.count());
        run.w.member("p50_ns", ns(50));
        run.w.member("p99_ns", ns(99));
        run.w.member("p999_ns", ns(99.9));
        run.w.member("max_ns", static_cast<double>(h.max()) / tpn);
        run.w.end_object();
    };
    run.w.key("risk");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", path.substr(path.find_last_of('/') + 1));
    run.w.member("messages", static_cast<std::uint64_t>(prefix));
    run.w.member("book_events", static_cast<std::uint64_t>(events.v.size()));
    run.w.member("instruments", static_cast<std::uint64_t>(traded.size()));
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("same_as_reference", same);
    run.w.member("itch_feed_handler_commit", TTT_ITCHFH_COMMIT);
    run.w.member("lockfree_ipc_commit", TTT_LFIPC_COMMIT);
    run.w.key("cases");
    run.w.begin_array();
    for (const auto& r : results) {
        run.w.begin_object();
        run.w.member("name", r->name);
        run.w.member("events_per_s", r->events_per_s);
        run.w.member("intents", r->intents);
        run.w.member("sent", r->sent);
        run.w.member("refused", r->refused);
        run.w.member("answers", r->answers);
        stats("event", r->event);
        stats("check", r->check);
        stats("answer", r->answer);
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(same, "risk_bench");
}
