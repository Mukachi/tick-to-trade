// The strategy's sink on the feed thread (note 002, S6), on CPU 2; the
// strategy's side of the ring (D4) a thread on CPU 3 that pops each event
// (separate cores, the same node). The first 50 million messages of the
// sample day (the open, 09:29:30 for five minutes, is messages 13.4 to 23.4
// million), decoded into memory first, through a fresh default book and
// handler in each case, as the feed handler's publish_bench (its note 008):
//   no sink         the handler with a sink that does nothing
//   publisher       the handler and its publisher, table only (no consumer)
//   sink, 1/10/100  the handler with the strategy's sink, D5's lists traded,
//                   each book event pushed into the ring
// Each message's handling timed between start() and stop(), unsubtracted, the
// first 10,000 discarded. The sink's cost is read beside "no sink". Book
// events sent, events set aside on a full ring and replaced are counted. Run
// JSON through lockfree-ipc's harness. Design: docs/design/002-book-events-strategy.md
//
// Usage: sink_bench [output.json] [day.gz]
#include "day_file.hpp"  // tools/

#include <ttt/event_ring.hpp>
#include <ttt/sink.hpp>

#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/publisher.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2, strategy_cpu = 3;
constexpr std::size_t prefix = 50'000'000, warmup = 10'000;
using hist = histogram<10, 40>;

struct quiet_sink {
    void book(const fh::applied&) {}
    void trade(const fh::trade_report&) {}
    void trade_broken(const fh::trade_report&) {}
    void status(std::uint16_t, fh::status_field) {}
    void imbalance(std::uint16_t, const fh::imbalance&) {}
    void market(const fh::message&) {}
    void directory(std::uint16_t, const fh::stock_directory&) {}
};

struct case_result {
    std::string name;
    int instruments = 0;
    double messages_per_s = 0;
    hist cost;
    std::uint64_t events = 0, set_aside = 0, replaced = 0;
};

std::vector<std::string> read_list(const std::string& path) {
    std::vector<std::string> v;
    std::ifstream f(path);
    for (std::string s; std::getline(f, s);)
        if (!s.empty()) v.push_back(s);
    return v;
}

// Counts what it pushes, then pushes into the ring.
struct counted_ring {
    ttt::event_ring<>& r;
    std::uint64_t pushed = 0;
    void push(const ttt::book_event& e) {
        ++pushed;
        r.push(e);
    }
    void flush() { r.flush(); }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : TTT_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "sink");
    if (run.refused) return run.finish(false, "sink_bench");
    const auto noise = sample_noise({cpu, strategy_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "sink_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "sink_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    std::vector<std::vector<std::string>> lists;
    for (const int n : {1, 10, 100}) {
        lists.push_back(read_list(TTT_CONFIG_DIR "/open-" + std::to_string(n) + ".txt"));
        if (lists.back().size() != static_cast<std::size_t>(n)) {
            std::fprintf(stderr, "sink_bench: config/open-%d.txt: %zu instruments\n", n, lists.back().size());
            return run.finish(false, "sink_bench");
        }
    }

    // The day's first messages, decoded, on CPU 2's node.
    std::vector<fh::message> msgs;
    msgs.reserve(prefix);
    {
        ttt::day_file day(path);
        fh::message m;
        while (day.open() && msgs.size() < prefix && day.next(m)) msgs.push_back(m);
    }
    if (msgs.size() < prefix) {
        std::fprintf(stderr, "sink_bench: %s: %zu messages read\n", path.c_str(), msgs.size());
        return run.finish(false, "sink_bench");
    }

    std::vector<std::unique_ptr<case_result>> results;
    bool ok = true;
    const std::string name = "/ttt-sb-" + std::to_string(::getpid());
    for (int c = -2; c < 3; ++c) {
        auto r = std::make_unique<case_result>();
        auto book = std::make_unique<fh::default_book>();
        const auto timed = [&](auto&& on) {
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < msgs.size(); ++i) {
                const ticks a = start();
                on(i + 1, msgs[i]);
                const ticks b = stop();
                if (i >= warmup) r->cost.record(b - a);
            }
            r->messages_per_s = static_cast<double>(msgs.size()) / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        };
        if (c == -2) {
            r->name = "no sink";
            quiet_sink sink;
            auto h = std::make_unique<fh::handler<fh::default_book, quiet_sink>>(*book, sink);
            timed([&](std::uint64_t, const fh::message& m) { h->on(m); });
        } else if (c == -1) {
            r->name = "publisher (table only)";
            auto p = std::make_unique<fh::published<>>(*book);
            std::string why;
            if (!p->pub.open(name, why)) {
                std::fprintf(stderr, "sink_bench: %s\n", why.c_str());
                return run.finish(false, "sink_bench");
            }
            timed([&](std::uint64_t seq, const fh::message& m) { p->on(seq, m); });
        } else {
            const std::vector<std::string>& l = lists[static_cast<std::size_t>(c)];
            r->instruments = static_cast<int>(l.size());
            r->name = "sink, " + std::to_string(l.size()) + (l.size() == 1 ? " instrument" : " instruments");
            const std::vector<std::string_view> traded(l.begin(), l.end());
            auto ins = std::make_unique<ttt::instruments>(traded);
            auto ring = std::make_unique<ttt::event_ring<>>();
            counted_ring out{*ring};
            auto f = std::make_unique<ttt::strategy_feed<counted_ring>>(*book, *ins, out);
            f->feed(fh::feed_state::live, 1);
            std::atomic<bool> done{false};
            std::thread strategy([&] {
                lfipc::bench::detail::pin_to(strategy_cpu);
                ttt::book_event e;
                for (;;)
                    if (!ring->pop(e) && done.load(std::memory_order_acquire) && !ring->pop(e)) break;
            });
            timed([&](std::uint64_t seq, const fh::message& m) { f->on(seq, m); });
            while (ring->waiting() > 0) ring->flush();
            done.store(true, std::memory_order_release);
            strategy.join();
            r->events = out.pushed;
            r->set_aside = ring->set_aside();
            r->replaced = ring->replaced();
        }
        const auto ns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
        std::printf("%-24s %.2f M messages/s, p50 %.0f ns, p99 %.0f ns, p99.9 %.0f ns a message; events %llu, set aside %llu, replaced %llu\n",
                    r->name.c_str(), r->messages_per_s / 1e6, ns(r->cost.percentile(50)), ns(r->cost.percentile(99)), ns(r->cost.percentile(99.9)),
                    static_cast<unsigned long long>(r->events), static_cast<unsigned long long>(r->set_aside),
                    static_cast<unsigned long long>(r->replaced));
        std::fflush(stdout);
        if (r->instruments > 0 && r->events == 0) ok = false;
        results.push_back(std::move(r));
    }

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
    run.w.key("sink");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("strategy_cpu", strategy_cpu);
    run.w.member("source", path.substr(path.find_last_of('/') + 1));
    run.w.member("messages", static_cast<std::uint64_t>(prefix));
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("ring_capacity", static_cast<std::uint64_t>(ttt::event_ring<>::ring::capacity));
    run.w.member("itch_feed_handler_commit", TTT_ITCHFH_COMMIT);
    run.w.member("lockfree_ipc_commit", TTT_LFIPC_COMMIT);
    run.w.key("cases");
    run.w.begin_array();
    for (const auto& r : results) {
        run.w.begin_object();
        run.w.member("name", r->name);
        run.w.member("instruments", r->instruments);
        run.w.member("messages_per_s", r->messages_per_s);
        run.w.member("events", r->events);
        run.w.member("set_aside", r->set_aside);
        run.w.member("replaced", r->replaced);
        run.w.key("cost");
        run.w.begin_object();
        run.w.member("count", r->cost.count());
        run.w.member("p50_ns", ns(r->cost.percentile(50)));
        run.w.member("p99_ns", ns(r->cost.percentile(99)));
        run.w.member("p999_ns", ns(r->cost.percentile(99.9)));
        run.w.member("max_ns", static_cast<double>(r->cost.max()) / tpn);
        run.w.end_object();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "sink_bench");
}
