// Tick to trade (note 005): from the time our sender meant to send the ITCH
// message that caused an order (t0, its stamp array) to the time the venue
// read the order (t4, order_venue's stamps), open loop, for each point of D3,
// split into stages: t1 the book event made (the sink, or the adapter in C),
// t2 the order written to the gateway's ring. (The journal's send record is
// stamped with the engine's pass, which can start before the order reaches
// the ring, so it cannot split t2 to t4: the gateway and the venue are one
// stage.) Only D and G caused by a book event are timed (D1); those caused by
// an answer are counted apart; the first 1,000 timed orders discarded.
// For each point it starts `ttt` (and `itchfh` for arrangement C; on loopback
// `order_venue` too), waits for its feed and session, drives the sender over
// its control port, waits for the program's end (note 004, D8), then joins the
// program's trace, the journal and the venue's list by ClOrdID. The sender is
// started first, by Shep, with the stamps /ttt-stamps (on the card, inside the
// far namespace with the venue). Run JSON through lockfree-ipc's harness.
// Design: docs/design/005-measurement.md
//
// Usage: t2t_bench loopback|card [output.json] [--quick]
//   --quick: a check run (not for results/): each point at 25k without the open, over 5 million messages
//   loopback: build/feed_sender 127.0.0.1 239.192.0.1 26400 26401 5 0 data/<day> /ttt-stamps
//   card:     inside itchfh-peer, the same at 10.99.0.2, and build/order_venue 10.99.0.2:26500 6 4 build/t2t-venue stamps
// The open (D3, amended): the morning at 50,000 packets a second, then the
// open (09:29:30 for five minutes) at the day's own pace; only orders caused
// by the open's messages are timed.
#include "programs.hpp"  // tests/

#include <ttt/block.hpp>

#include <fixgw/journal.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include "stamps.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <signal.h>

namespace {

using namespace lfipc::bench;
using namespace ttt::test;

constexpr const char* stamps_name = "/ttt-stamps";
constexpr std::uint64_t prefix = 50'000'000;  // the fixed paces' messages (D3)
constexpr std::uint64_t warmup = 1'000;       // timed orders discarded at the start of each point
constexpr std::uint64_t open_from_ns = (9ull * 3600 + 29 * 60 + 30) * 1'000'000'000, open_to_ns = open_from_ns + 300ull * 1'000'000'000;

std::int64_t mono() {
    timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1'000'000'000LL + t.tv_nsec;
}

struct point {
    const char* name;
    char arrangement, journal;
    double pace;  // packets a second; 0: the open at the day's own pace, after the morning at 50k (D3, amended)
};
constexpr double lead_pace = 50'000;

struct percentiles {
    std::size_t count = 0;
    double p50 = -1, p99 = -1, p999 = -1, max = -1;
};
percentiles of(std::vector<std::int64_t>& v) {
    percentiles p;
    p.count = v.size();
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return static_cast<double>(v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]); };
    p.p50 = at(0.5), p.p99 = at(0.99), p.p999 = at(0.999), p.max = static_cast<double>(v.back());
    return p;
}

struct result {
    point pt;
    bool complete = false, s1 = false;
    std::uint64_t first = 0, last = 0;  // the messages timed
    std::uint64_t orders = 0, by_event = 0, by_answer = 0, unjoined = 0, kernel_drops = ~0ull, stage_mismatch = 0;
    percentiles total, feed, strategy, gateway;  // t0-t4; t0-t1, t1-t2, t2-t4
    itchfh::sender_counts sent{};
    std::string summary;
};

std::uint64_t number_before(const std::string& text, const std::string& suffix) {
    const std::size_t at = text.find(suffix);
    if (at == std::string::npos) return ~0ull;
    std::size_t from = at;
    while (from > 0 && text[from - 1] >= '0' && text[from - 1] <= '9') --from;
    return from == at ? ~0ull : std::stoull(text.substr(from, at - from));
}

// The long form of R (the feed handler's e2e_bench): answered with the first and last messages it resolves to.
bool run_sender(sender_control& ctl, const char (&session)[11], double pace, double speed, std::uint64_t last, std::uint64_t from, std::uint64_t to,
                std::uint64_t (&span)[2]) {
    unsigned char b[59];
    const double drop = 0;
    b[0] = 'R';
    std::memcpy(b + 1, session, 10);
    std::memcpy(b + 11, &pace, 8);
    std::memcpy(b + 19, &drop, 8);
    std::memcpy(b + 27, &last, 8);
    std::memcpy(b + 35, &speed, 8);
    std::memcpy(b + 43, &from, 8);
    std::memcpy(b + 51, &to, 8);
    ::sendto(ctl.fd, b, sizeof(b), 0, reinterpret_cast<const sockaddr*>(&ctl.to), sizeof(ctl.to));
    return ::recv(ctl.fd, span, sizeof(span), 0) == static_cast<ssize_t>(sizeof(span));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "";
    if (path != "loopback" && path != "card") {
        std::fprintf(stderr, "usage: t2t_bench loopback|card [output.json] [--quick]\n");
        return 2;
    }
    const bool card = path == "card";
    const bool quick = argc > 3 && std::string(argv[3]) == "--quick";
    char* run_argv[] = {argv[0], argc > 2 ? argv[2] : nullptr, nullptr};
    run_file run(argc > 2 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "t2t-" + path);
    if (run.refused) return run.finish(false, "t2t_bench");
    if (quick && run.published) {
        std::fprintf(stderr, "t2t_bench: --quick is for check runs, not results/\n");
        return run.finish(false, "t2t_bench");
    }
    const auto noise = sample_noise({2, 3, 4}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "t2t_bench: refused: %s\n", clk.reason.c_str());
        return run.finish(false, "t2t_bench");
    }
    std::string why;
    const itchfh::bench::stamp_array* st = itchfh::bench::open_stamps(stamps_name, false, why);
    sender_control ctl(26402);
    if (card) ::inet_pton(AF_INET, "10.99.0.2", &ctl.to.sin_addr);
    itchfh::sender_counts probe{};
    if (!st || !ctl.stop(probe)) {
        std::fprintf(stderr, "t2t_bench: feed_sender with stamps %s does not answer at %s:26402 (%s)\n", stamps_name, card ? "10.99.0.2" : "127.0.0.1",
                     why.c_str());
        return run.finish(false, "t2t_bench");
    }

    // D2, S3: the measurement's own cost, each alone: a clock read, a stamp read.
    std::vector<std::int64_t> clock_cost, stamp_cost;
    lfipc::bench::detail::pin_to(3);  // the strategy thread's CPU, before the points use it
    {
        clock_cost.reserve(1'000'000);
        stamp_cost.reserve(1'000'000);
        const double tpn = clk.cal.ticks_per_ns;
        for (int i = 0; i < 1'000'000; ++i) {
            const ticks a = start();
            [[maybe_unused]] volatile std::int64_t t = mono();
            const ticks b = lfipc::bench::stop();
            clock_cost.push_back(static_cast<std::int64_t>(static_cast<double>(b - a) / tpn));
        }
        for (std::uint64_t i = 0; i < 1'000'000; ++i) {
            const ticks a = start();
            [[maybe_unused]] volatile std::int64_t t = st->get(i * 7919);
            const ticks b = lfipc::bench::stop();
            stamp_cost.push_back(static_cast<std::int64_t>(static_cast<double>(b - a) / tpn));
        }
    }
    std::vector<std::int64_t> pair_alone;
    for (int i = 0; i < 1'000'000; ++i) {
        const ticks a = start();
        const ticks b = lfipc::bench::stop();
        pair_alone.push_back(static_cast<std::int64_t>(static_cast<double>(b - a) / clk.cal.ticks_per_ns));
    }

    lfipc::bench::detail::pin_to(1);  // housekeeping: the benchmark only waits; the programs pin themselves
    std::vector<point> points;
    points.push_back({"A, journal C, 10k", 'A', 'C', 10'000});
    points.push_back({"A, journal C, 25k", 'A', 'C', 25'000});
    points.push_back({"A, journal C, 50k", 'A', 'C', 50'000});
    points.push_back({"A, journal C, the open", 'A', 'C', 0});
    if (!card) {
        points.push_back({"B, journal C, 25k", 'B', 'C', 25'000});
        points.push_back({"B, journal C, the open", 'B', 'C', 0});
        points.push_back({"C, journal C, 25k", 'C', 'C', 25'000});
        points.push_back({"C, journal C, the open", 'C', 'C', 0});
        points.push_back({"A, journal A, 25k", 'A', 'A', 25'000});
        points.push_back({"A, journal A, the open", 'A', 'A', 0});
    }
    if (quick) {  // a check run: every arrangement and journal over 5 million messages, no open
        std::vector<point> q;
        for (const point& p : points)
            if (p.pace != 0 && (p.pace == 25'000 || p.arrangement != 'A' || p.journal != 'C')) q.push_back(p);
        points = q;
    }

    const std::string tag = std::to_string(::getpid());
    const std::string dir = std::string(TTT_BUILD_DIR) + "/t2t-" + tag, venue_dir = card ? std::string(TTT_BUILD_DIR) + "/t2t-venue" : dir;
    std::filesystem::create_directories(dir);
    const std::string feed = dir + "/feed.conf", pub = "/ttt-t2t-" + tag + "-pub", block = "/ttt-t2t-" + tag;
    std::ofstream(feed) << "# t2t_bench\nport = 26400\nserver_port = 26401\nsnapshot_port = 0\ncpu = 2\nrebuild_cpu = 1\npublish = " << pub
                        << "\nmonitor = none\nrecord_dir = none\n" << (card ? "interface = 10.99.0.1\nserver = 10.99.0.2\n" : "");
    std::vector<result> results;
    int k = 0;
    for (const point& pt : points) {
        result r;
        r.pt = pt;
        const std::string conf = dir + "/ttt-" + std::to_string(k) + ".conf", out = dir + "/ttt-" + std::to_string(k) + ".txt";
        const std::string trace = dir + "/trace-" + std::to_string(k) + ".txt", passed = dir + "/passed-" + std::to_string(k) + ".txt";
        const std::string journal = pt.journal == 'C' ? "/dev/shm/ttt-t2t-" + tag + ".journal" : dir + "/journal-" + std::to_string(k) + ".journal";
        std::filesystem::remove(journal);
        std::ofstream(conf) << "arrangement = " << pt.arrangement << "\nfeed = feed.conf\nstrategy_cpu = 3\nengine_cpu = 4\njournal_cpu = 7\nsender_cpu = 8\n"
                            << "venue = " << (card ? "10.99.0.2" : "127.0.0.1") << "\nvenue_port = 26500\njournal = " << pt.journal
                            << (pt.journal == 'A' ? "\nengine = pipeline" : "") << "\njournal_path = " << journal
                            << "\njournal_mb = " << (pt.journal == 'C' ? 4096 : 1024) << "\ninstruments = " TTT_CONFIG_DIR "/open-100.txt\nlimits = " TTT_CONFIG_DIR
                            << "/limits.conf\nblock = " << block << "\npassed_log = " << passed << "\nstamps = " << stamps_name << "\ntrace = " << trace << "\n";
        const int venue_k = card ? k : 0;
        const std::string venue_orders = venue_dir + "/venue-" + std::to_string(venue_k) + ".orders";
        std::filesystem::remove(venue_orders);
        pid_t venue = -1, itchfh = -1;
        if (!card) venue = spawn({TTT_VENUE, "26500", "6", "1", venue_dir, "stamps"}, dir + "/venue-" + std::to_string(k) + ".log");
        if (pt.arrangement == 'C') {
            itchfh = spawn({TTT_ITCHFH, feed}, dir + "/itchfh-" + std::to_string(k) + ".txt");
            for (int i = 0; i < 600 && !std::filesystem::exists("/dev/shm" + pub); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const pid_t program = spawn({TTT_PROGRAM, conf}, out);
        ttt::mon::block b;
        bool attached = false;
        for (int i = 0; i < 600 && !(attached = b.attach(block, why)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ttt::mon::values v{};
        for (int i = 0; attached && i < 1200 && !(v[ttt::mon::logged_on] && v[ttt::mon::feed_ready]); ++i) {
            b.read(v);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!v[ttt::mon::logged_on] || !v[ttt::mon::feed_ready]) {  // nothing would be timed: say why, and skip the point
            std::printf("%-20s INCOMPLETE; %s within 60 s (is the venue listening at %s:26500?)\n", pt.name,
                        !attached ? "the program's block never appeared" : !v[ttt::mon::logged_on] ? "never logged on" : "the feed never ready",
                        card ? "10.99.0.2" : "127.0.0.1");
            std::fflush(stdout);
            ::kill(program, SIGTERM);
            wait_exit_for(program, 60 * 1000);
            if (itchfh > 0) {
                ::kill(itchfh, SIGTERM);
                wait_exit_for(itchfh, 60 * 1000);
            }
            if (venue > 0) {
                ::kill(venue, SIGTERM);
                wait_exit_for(venue, 60 * 1000);
            }
            results.push_back(r);
            std::filesystem::remove(journal);
            ++k;
            continue;
        }
        char session[11] = "TTT2T00000";
        session[8] = static_cast<char>('0' + k / 10);
        session[9] = static_cast<char>('0' + k % 10);
        const bool open = pt.pace == 0;
        std::uint64_t span[2] = {0, 0};  // the messages whose orders are timed
        const bool driven = run_sender(ctl, session, open ? lead_pace : pt.pace, open ? 1 : 0, open ? 0 : (quick ? 5'000'000 : prefix),
                                       open ? open_from_ns : 0, open ? open_to_ns : 0, span);
        r.first = span[0], r.last = span[1];
        const int exit_code = wait_exit_for(program, 20 * 60 * 1000);
        const int itchfh_exit = itchfh > 0 ? wait_exit_for(itchfh, 60 * 1000) : 0;
        const int venue_exit = venue > 0 ? wait_exit_for(venue, 60 * 1000) : 0;
        for (int i = 0; card && i < 600 && !std::filesystem::exists(venue_orders); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ctl.stop(r.sent);
        const std::string text = read_file(out), feed_text = pt.arrangement == 'C' ? read_file(dir + "/itchfh-" + std::to_string(k) + ".txt") : text;
        r.kernel_drops = number_before(feed_text, " kernel drops");
        r.complete = driven && exit_code == 0 && itchfh_exit == 0 && venue_exit == 0 && text.find("every order answered; logged out") != std::string::npos;
        std::string summary = text.find("ttt: arrangement " + std::string(1, pt.arrangement) + ";") != std::string::npos
                                  ? text.substr(text.find("ttt: arrangement " + std::string(1, pt.arrangement) + ";"))
                                  : text;
        while (!summary.empty() && summary.back() == '\n') summary.pop_back();
        r.summary = summary;

        // The join: the program's trace, the journal's send records (t3), the venue's reads (t4), by ClOrdID.
        std::vector<sent_order> sent;
        const bool journal_read = journal_orders(journal, sent);
        std::unordered_map<std::uint64_t, std::int64_t> t3, t4;
        t3.reserve(sent.size() * 2);
        for (const sent_order& o : sent) t3[o.id] = o.mono_ns;
        {
            std::ifstream f(venue_orders);
            std::int64_t ns;
            char type;
            std::string id;
            while (f >> ns >> type >> id)
                if (!id.empty() && id[0] >= '0' && id[0] <= '9') t4[std::stoull(id)] = ns;
        }
        std::vector<std::int64_t> total, s_feed, s_strategy, s_gateway;
        {
            std::ifstream f(trace);
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("lost", 0) == 0) {
                    r.unjoined += std::stoull(line.substr(5));
                    continue;
                }
                std::istringstream in(line);
                std::uint64_t id, seq;
                char type;
                std::int64_t t0, t1, t2;
                if (!(in >> id >> type >> seq >> t0 >> t1 >> t2)) continue;
                ++r.orders;
                if (seq == 0) {
                    ++r.by_answer;
                    continue;
                }
                if (seq < r.first || seq > r.last) continue;  // the open's morning: not timed
                ++r.by_event;
                const auto a = t3.find(id), z = t4.find(id);
                if (t0 <= 0 || t1 <= 0 || a == t3.end() || z == t4.end()) {
                    ++r.unjoined;
                    continue;
                }
                if (r.by_event <= warmup) continue;
                total.push_back(z->second - t0);
                s_feed.push_back(t1 - t0);
                s_strategy.push_back(t2 - t1);
                s_gateway.push_back(z->second - t2);
                if (t1 < t0 || t2 < t1 || z->second < t2) ++r.stage_mismatch;  // out of order: a clock or a join wrong
            }
        }
        r.total = of(total), r.feed = of(s_feed), r.strategy = of(s_strategy), r.gateway = of(s_gateway);
        // Note 004's S1 in this run.
        const std::vector<std::uint64_t> ids = read_ids(passed);
        std::size_t j = 0;
        for (std::size_t i = 0; i < ids.size() && j < sent.size(); ++i)
            if (ids[i] == sent[j].id) ++j;
        r.s1 = journal_read && j == sent.size() && !sent.empty();
        std::printf("%-20s %s; %llu orders, %llu caused by a book event, %llu by an answer, %llu unjoined; tick to trade p50 %.0f p99 %.0f p99.9 %.0f "
                    "max %.0f ns; stages p50 feed %.0f, strategy %.0f, gateway and venue %.0f; %llu out of order; %llu kernel drops; sender late max %lld ns\n",
                    pt.name, r.complete && r.s1 ? "complete" : "INCOMPLETE", static_cast<unsigned long long>(r.orders),
                    static_cast<unsigned long long>(r.by_event), static_cast<unsigned long long>(r.by_answer), static_cast<unsigned long long>(r.unjoined),
                    r.total.p50, r.total.p99, r.total.p999, r.total.max, r.feed.p50, r.strategy.p50, r.gateway.p50,
                    static_cast<unsigned long long>(r.stage_mismatch),
                    static_cast<unsigned long long>(r.kernel_drops), static_cast<long long>(r.sent.late_max_ns));
        std::fflush(stdout);
        results.push_back(r);
        std::filesystem::remove(journal);
        ++k;
    }

    write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    const auto stats = [&](const char* key, const percentiles& p) {
        run.w.key(key);
        run.w.begin_object();
        run.w.member("count", static_cast<std::uint64_t>(p.count));
        run.w.member("p50_ns", p.p50);
        run.w.member("p99_ns", p.p99);
        run.w.member("p999_ns", p.p999);
        run.w.member("max_ns", p.max);
        run.w.end_object();
    };
    bool ok = true;
    run.w.key("t2t");
    run.w.begin_object();
    run.w.member("path", path);
    // On the card, kernel busy polling as the feed handler's runs set it (its note 007: system-wide, 50).
    const auto sysctl = [](const char* f) {
        std::ifstream in(f);
        std::uint64_t x = 0;
        in >> x;
        return x;
    };
    run.w.member("busy_read", sysctl("/proc/sys/net/core/busy_read"));
    run.w.member("busy_poll", sysctl("/proc/sys/net/core/busy_poll"));
    run.w.member("source", "01302020.NASDAQ_ITCH50.gz");
    run.w.member("messages", quick ? std::uint64_t{5'000'000} : prefix);
    run.w.member("instruments", std::uint64_t{100});
    run.w.member("warmup_discarded", warmup);
    run.w.member("cpus", "feed 2, strategy 3, engine 4, sender 5, venue 6, pipeline journal 7 and sender 8");
    run.w.member("itch_feed_handler_commit", TTT_ITCHFH_COMMIT);
    run.w.member("fix_order_gateway_commit", TTT_FIXGW_COMMIT);
    run.w.member("lockfree_ipc_commit", TTT_LFIPC_COMMIT);
    percentiles cc = of(clock_cost), sc = of(stamp_cost), pa = of(pair_alone);
    stats("cost_clock_read", cc);
    stats("cost_stamp_read", sc);
    stats("cost_timer_pair_alone", pa);
    run.w.key("points");
    run.w.begin_array();
    for (const result& r : results) {
        ok = ok && r.complete && r.s1 && r.unjoined == 0 && r.stage_mismatch == 0;
        run.w.begin_object();
        run.w.member("name", r.pt.name);
        run.w.member("arrangement", std::string(1, r.pt.arrangement));
        run.w.member("journal", std::string(1, r.pt.journal));
        run.w.member("pace", r.pt.pace);
        run.w.member("first_message", r.first);
        run.w.member("last_message", r.last);
        run.w.member("complete", r.complete);
        run.w.member("s1", r.s1);
        run.w.member("orders", r.orders);
        run.w.member("by_event", r.by_event);
        run.w.member("by_answer", r.by_answer);
        run.w.member("unjoined", r.unjoined);
        run.w.member("stage_mismatch", r.stage_mismatch);
        run.w.member("kernel_drops", r.kernel_drops);
        run.w.member("sender_late_max_ns", r.sent.late_max_ns);
        run.w.member("sender_late_over_10us", r.sent.late_over_10us);
        run.w.member("sender_packets", r.sent.packets);
        run.w.member("summary", r.summary);
        stats("tick_to_trade", r.total);
        stats("stage_feed", r.feed);
        stats("stage_strategy", r.strategy);
        stats("stage_gateway_venue", r.gateway);
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    std::printf("measurement's cost p50: clock read %.0f ns, stamp read %.0f ns (the timer pair alone %.0f)\n", cc.p50, sc.p50, pa.p50);
    if (ok) std::filesystem::remove_all(dir);
    return run.finish(ok, "t2t_bench");
}
