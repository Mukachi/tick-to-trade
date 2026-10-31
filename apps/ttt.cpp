// The program (note 004): the feed and its books, the strategy and the risk
// checks, the gateway's engine and session to the venue, in one of note 001's
// three arrangements (D4), from one configuration file:
//   A  feed thread (the feed handler's feed_one and recovery, the strategy's
//      sink) -> event ring -> strategy thread (trader.hpp) -> the gateway's
//      application ring -> engine thread
//   B  the feed thread does the strategy thread's work in the sink's push
//   C  a consumer process of `itchfh`'s publisher, through note 002's
//      adapter, on the strategy thread (start it before the sender: open item T1)
// Each run is a new session (D6: an existing journal is refused). At the
// feed's end, or on `ttt-ctl stop` (SIGINT and SIGTERM send it), D8: the kill
// switch engaged, the answers awaited, the gateway's logout, the counts.
// Signals are blocked on every thread but one that waits for them (D5).
// Design: docs/design/004-program.md
//
// Usage: ttt <config>
#include <ttt/adapter.hpp>
#include <ttt/block.hpp>
#include <ttt/config.hpp>
#include <ttt/event_ring.hpp>
#include <ttt/limits.hpp>
#include <ttt/sink.hpp>
#include <ttt/trader.hpp>

#include <itchfh/consumer.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/monitoring.hpp>
#include <itchfh/recording.hpp>
#include <itchfh/recovery.hpp>
#include <itchfh/udp.hpp>

#include <fixgw/durability.hpp>
#include <fixgw/engine.hpp>
#include <fixgw/pipeline.hpp>

#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/cross_cpu.hpp>

#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fh = itchfh;
namespace fx = fixgw;

struct quiet_policy {
    void log(fx::session_log l, std::string_view what, std::uint64_t) {
        if (l != fx::session_log::info) std::fprintf(stderr, "ttt: gateway: %.*s\n", static_cast<int>(what.size()), what.data());
    }
    bool resend(const fx::message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

std::int64_t mono_ns() { return fh::monotonic_ns(); }

// The feed handler's receiver and recovery, as `itchfh` opens them.
bool open_receiver(const fh::program_config& f, fh::udp::receiver& rx, std::string& why) {
    fh::udp::receiver::options ro;
    ro.group = f.group;
    ro.interface_ip = f.interface_ip;
    ro.port = f.port;
    ro.buffer_bytes = f.buffer_bytes;
    ro.busy_poll_us = f.busy_poll_us;
    if (!rx.open(ro, why)) return false;
    rx.set_server(f.server_ip, f.server_port);
    return true;
}
fh::recovery_config recovery_of(const fh::program_config& f) {
    fh::recovery_config rc;
    rc.ip = f.snapshot_ip;
    rc.port = f.snapshot_port;
    rc.rebuild_cpu = f.rebuild_cpu;
    rc.join = f.join_snapshot && f.snapshot_port != 0;
    return rc;
}

struct shared {
    std::atomic<bool> feed_ready{false}, feed_ended{false}, stopping{false};
    std::atomic<std::uint64_t> set_aside{0}, replaced{0};
};

// D8, on the strategy thread: true if every order was answered and the session logged out.
template <class Trader, class Engine>
bool finish(Trader& t, Engine& gw, fx::app_ring& to_gw, ttt::mon::block& b, const ttt::program_config& c, bool& quiet) {
    t.end();
    const std::int64_t answer_by = mono_ns() + static_cast<std::int64_t>(c.end_wait_ms) * 1'000'000;
    while (!t.quiet() && mono_ns() < answer_by) t.pass(&b);
    quiet = t.quiet();
    while (!fx::submit_logout(to_gw)) {
    }
    const std::int64_t out_by = mono_ns() + 15'000'000'000;
    while (!gw.finished() && mono_ns() < out_by) t.pass(&b);
    t.refresh(b);
    return quiet && gw.finished();
}

template <class Engine>
int trade(const ttt::program_config& c, Engine& gw, fx::app_ring& to_gw, fx::app_ring& from_gw, ttt::instruments& ins,
          const ttt::limits& lim, double tpn, ttt::mon::block& b) {
    using trader_t = ttt::trader<Engine>;
    auto t = std::make_unique<trader_t>(ins, lim, gw, to_gw, from_gw, tpn, std::size_t{64} << 20, c.arrangement);
    shared sh;
    bool quiet = false, ok = false;
    fh::mon::block feed_mon;
    std::string why;
    const bool feed_monitored = c.arrangement != 'C' && !c.feed_config.monitor.empty();
    if (feed_monitored && !feed_mon.create(c.feed_config.monitor, why)) {
        std::fprintf(stderr, "ttt: feed monitor: %s\n", why.c_str());
        return 1;
    }
    fh::book_config bc;
    bc.orders = c.feed_config.orders;
    bc.levels = c.feed_config.levels;
    fh::sequencer_config qc;
    qc.hold_bytes = static_cast<std::size_t>(c.feed_config.hold_mb) << 20;

    // The feed thread's loop (A, B): until the feed ends or the strategy thread stops.
    const auto feed_loop = [&](auto& out, auto&& each_pass) -> bool {
        using out_t = std::remove_reference_t<decltype(out)>;
        using sink_t = ttt::sink<fh::default_book, out_t>;
        if (!lfipc::bench::detail::pin_to(c.feed_config.cpu)) {
            std::fprintf(stderr, "ttt: cannot pin the feed thread to CPU %d\n", c.feed_config.cpu);
            return false;
        }
        fh::udp::receiver rx;
        std::string w;
        if (!open_receiver(c.feed_config, rx, w)) {
            std::fprintf(stderr, "ttt: %s\n", w.c_str());
            return false;
        }
        auto sk = std::make_unique<sink_t>(ins, out);
        auto rcv = std::make_unique<fh::recovery<fh::default_book, sink_t>>(*sk, bc, fh::handler_config{}, recovery_of(c.feed_config));
        fh::feed_one<fh::published_on<fh::default_book, sink_t>> f(rx, rcv->active(), qc, c.feed_config.batch);
        rcv->begin(f);
        sh.feed_ready.store(true, std::memory_order_release);
        std::fprintf(stderr, "ttt: feed ready\n");
        fh::mon::values v{};
        std::int64_t last_mon = 0;
        while (!sh.stopping.load(std::memory_order_relaxed) && f.seq().state() != fh::feed_state::ended) {
            f.poll();
            const std::int64_t now = mono_ns();
            if (c.feed_config.snapshot_port != 0 && rcv->step(f, now)) fh::recovery<fh::default_book, sink_t>::drain(f);
            if (!each_pass()) break;
            if (now - last_mon >= 100'000'000) {
                if (feed_monitored) {
                    fh::mon::gather(v, f, rcv->book(), static_cast<const fh::publisher<fh::default_book>*>(nullptr),
                                    static_cast<const fh::rec::recorder*>(nullptr), now);
                    fh::mon::gather_recovery(v, *rcv);
                    feed_mon.write(v);
                }
                if constexpr (requires { out.set_aside(); }) {
                    sh.set_aside.store(out.set_aside(), std::memory_order_relaxed);
                    sh.replaced.store(out.replaced(), std::memory_order_relaxed);
                }
                last_mon = now;
            }
        }
        if (f.seq().state() == fh::feed_state::ended) sh.feed_ended.store(true, std::memory_order_release);
        std::fprintf(stderr, "ttt: feed %s: %llu datagrams, %llu messages, %llu kernel drops, %llu gaps, %llu snapshots\n",
                     f.seq().state() == fh::feed_state::ended ? "ended" : "stopped", static_cast<unsigned long long>(f.counts().datagrams),
                     static_cast<unsigned long long>(f.counts().messages), static_cast<unsigned long long>(f.counts().kernel_drops),
                     static_cast<unsigned long long>(f.seq().counts().gaps), static_cast<unsigned long long>(rcv->counts().snapshots));
        return true;
    };

    if (c.arrangement == 'A') {
        auto ring = std::make_unique<ttt::event_ring<>>();
        bool feed_ok = false;
        std::thread feed([&] { feed_ok = feed_loop(*ring, [] { return true; }); });
        if (!lfipc::bench::detail::pin_to(c.strategy_cpu)) std::fprintf(stderr, "ttt: cannot pin the strategy thread to CPU %d\n", c.strategy_cpu);
        ttt::book_event e;
        bool ready = false;
        for (;;) {
            while (ring->pop(e)) t->push(e);
            if (!ready && (ready = sh.feed_ready.load(std::memory_order_acquire))) t->feed_ready();
            if (!t->pass(&b)) break;
            if (sh.feed_ended.load(std::memory_order_acquire)) {
                while (ring->pop(e)) t->push(e);
                t->feed_ended();
                break;
            }
            t->ring_counts(sh.set_aside.load(std::memory_order_relaxed), sh.replaced.load(std::memory_order_relaxed));
        }
        sh.stopping.store(true, std::memory_order_relaxed);
        ok = finish(*t, gw, to_gw, b, c, quiet);
        feed.join();
        ok = ok && feed_ok;
    } else if (c.arrangement == 'B') {
        bool told_stop = false;
        const bool feed_ok = feed_loop(*t, [&] {
            t->feed_ready();
            if (t->pass(&b)) return true;
            told_stop = true;
            return false;
        });
        if (sh.feed_ended.load(std::memory_order_acquire)) t->feed_ended();
        (void)told_stop;
        ok = finish(*t, gw, to_gw, b, c, quiet) && feed_ok;
    } else {
        // C: a consumer of `itchfh`'s publisher.
        if (!lfipc::bench::detail::pin_to(c.strategy_cpu)) std::fprintf(stderr, "ttt: cannot pin to CPU %d\n", c.strategy_cpu);
        fh::pub::consumer con;
        if (!con.attach(c.feed_config.publish, why)) {
            std::fprintf(stderr, "ttt: %s\n", why.c_str());
            return 1;
        }
        con.report_node();
        ttt::adapter<trader_t> ad(ins, *t);
        const auto read = [&](std::uint16_t l, fh::pub::entry_view& v) { return con.read(l, v); };
        bool live = false;
        fh::pub::update u;
        std::uint64_t rejoins = 0;
        for (;;) {
            while (con.poll(u)) ad.on(u);
            if (!live && con.state() == fh::pub::slot_live) {
                ad.joined(read);
                live = true;
                t->feed_ready();
            }
            if (con.overrun()) {
                con.rejoin();
                live = false;
                ++rejoins;
            }
            if (!t->pass(&b)) break;
            std::uint64_t next = 0;
            if (live && con.feed(next) == fh::feed_state::ended) {
                while (con.poll(u)) ad.on(u);
                t->feed_ended();
                break;
            }
        }
        if (rejoins) std::fprintf(stderr, "ttt: %llu rejoins after an overrun\n", static_cast<unsigned long long>(rejoins));
        ok = finish(*t, gw, to_gw, b, c, quiet);
    }

    const ttt::risk_counts& rc = t->risk_counters();
    const ttt::strategy_counts& sc = t->strategy_counters();
    std::uint64_t refused = 0;
    for (const std::uint64_t n : rc.refused) refused += n;
    std::printf("ttt: arrangement %c; %llu book events; %llu intents (%llu new, %llu replace, %llu cancel); %llu passed, %llu refused; "
                "%llu answers, %llu unmapped, %llu refused by the gateway; %llu fills; kill engaged %llu times, %llu kill cancels; "
                "%s; %s\n",
                c.arrangement, static_cast<unsigned long long>(t->counters()[ttt::mon::events]),
                static_cast<unsigned long long>(sc.news + sc.replaces + sc.cancels), static_cast<unsigned long long>(sc.news),
                static_cast<unsigned long long>(sc.replaces), static_cast<unsigned long long>(sc.cancels),
                static_cast<unsigned long long>(rc.passed), static_cast<unsigned long long>(refused),
                static_cast<unsigned long long>(t->counters()[ttt::mon::answers]), static_cast<unsigned long long>(t->counters()[ttt::mon::unmapped]),
                static_cast<unsigned long long>(t->counters()[ttt::mon::gateway_refused]), static_cast<unsigned long long>(rc.fills),
                static_cast<unsigned long long>(rc.kills), static_cast<unsigned long long>(rc.kill_cancels),
                quiet ? "every order answered" : "ORDERS LEFT UNANSWERED", gw.finished() ? "logged out" : "NOT LOGGED OUT");
    if (t->counters()[ttt::mon::gateway_refused])
        std::printf("ttt: the gateway's last refusal: %.*s\n", static_cast<int>(t->last_gateway_refusal().size()), t->last_gateway_refusal().data());
    if (!c.passed_log.empty()) {
        std::ofstream f(c.passed_log);
        for (const std::uint64_t id : t->passed()) f << id << '\n';
        if (t->passed_lost()) f << "lost " << t->passed_lost() << '\n';
    }
    return ok && t->passed_lost() == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: ttt <config>\n");
        return 2;
    }
    ttt::program_config c;
    std::string why;
    if (!ttt::load_program(argv[1], c, why)) {
        std::fprintf(stderr, "ttt: %s\n", why.c_str());
        return 2;
    }
    if (c.pipeline && c.journal != 'A') {
        std::fprintf(stderr, "ttt: the gateway's pipeline journals with A (its note 005): set journal = A\n");
        return 2;
    }
    std::vector<std::string> names;
    {
        std::ifstream f(c.instruments);
        for (std::string s; std::getline(f, s);)
            if (!s.empty()) names.push_back(s);
    }
    ttt::limits lim;
    if (names.empty() || !ttt::read_limits(c.limits, lim, why)) {
        std::fprintf(stderr, "ttt: %s: %zu instruments; %s\n", c.instruments.c_str(), names.size(), why.c_str());
        return 2;
    }
    // D6: a new session each run.
    if (std::filesystem::exists(c.journal_path)) {
        std::fprintf(stderr, "ttt: %s exists: each run is a new session (note 004, D6); remove it first\n", c.journal_path.c_str());
        return 2;
    }

    // D5: signals to one thread, which turns them into the stop command.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    ttt::mon::block b;
    if (!b.create(c.block, why)) {
        std::fprintf(stderr, "ttt: %s\n", why.c_str());
        return 1;
    }
    std::thread([&b, sigs] {
        int s = 0;
        while (::sigwait(&sigs, &s) == 0) b.send(ttt::mon::stop);
    }).detach();

    const auto cal = lfipc::bench::calibrate();
    if (!cal.ok) {
        std::fprintf(stderr, "ttt: TSC calibration: %s\n", cal.reason.c_str());
        return 1;
    }
    const std::vector<std::string_view> traded(names.begin(), names.end());
    ttt::instruments ins(traded);

    fx::session_config sc;
    sc.role = fx::session_role::initiator;
    sc.sender_comp_id = c.sender_comp_id;
    sc.target_comp_id = c.target_comp_id;
    fx::engine_config ec;
    ec.port = c.venue_port;
    in_addr a{};
    ::inet_pton(AF_INET, c.venue_ip.c_str(), &a);
    ec.ip = ntohl(a.s_addr);
    fx::order_config oc;
    oc.max_orders = c.max_orders;
    oc.max_exec_ids = c.max_exec_ids;
    oc.id_bytes = static_cast<std::size_t>(c.id_mb) << 20;
    auto to_gw = std::make_unique<fx::app_ring>(), from_gw = std::make_unique<fx::app_ring>();
    quiet_policy policy;
    std::atomic<bool> stop_engine{false};
    std::fprintf(stderr, "ttt: arrangement %c, %zu instruments, venue %s:%u, journal %c (%s), %s engine on CPU %d\n", c.arrangement, ins.size(),
                 c.venue_ip.c_str(), c.venue_port, c.journal, c.journal_path.c_str(), c.pipeline ? "pipeline" : "direct", c.engine_cpu);
    const std::size_t journal_bytes = static_cast<std::size_t>(c.journal_mb) << 20;
    int rc = 1;
    if (c.pipeline) {
        fx::journal_file jf;
        if (!jf.create(c.journal_path, 1, journal_bytes)) {
            std::fprintf(stderr, "ttt: cannot create %s\n", c.journal_path.c_str());
            return 1;
        }
        auto gw = std::make_unique<fx::pipeline_engine<quiet_policy>>(sc, ec, jf, policy, *to_gw, *from_gw,
                                                                       fx::pipeline_engine<quiet_policy>::cpus{c.engine_cpu, c.journal_cpu, c.sender_cpu}, oc);
        std::thread et([&] { gw->run(stop_engine); });
        rc = trade(c, *gw, *to_gw, *from_gw, ins, lim, cal.ticks_per_ns, b);
        stop_engine = true;
        et.join();
    } else {
        const auto direct = [&](auto& journal) {
            using journal_t = std::remove_reference_t<decltype(journal)>;
            auto gw = std::make_unique<fx::engine<journal_t, quiet_policy>>(sc, ec, journal, policy, *to_gw, *from_gw, oc);
            std::thread et([&] {
                if (!lfipc::bench::detail::pin_to(c.engine_cpu)) std::fprintf(stderr, "ttt: cannot pin the engine to CPU %d\n", c.engine_cpu);
                gw->run(stop_engine);
            });
            const int r = trade(c, *gw, *to_gw, *from_gw, ins, lim, cal.ticks_per_ns, b);
            stop_engine = true;
            et.join();
            return r;
        };
        if (c.journal == 'C') {
            fx::mapped_journal j;
            if (!j.create(c.journal_path, 1, journal_bytes)) {
                std::fprintf(stderr, "ttt: cannot create %s\n", c.journal_path.c_str());
                return 1;
            }
            rc = direct(j);
        } else {
            fx::fsync_each j;
            if (!j.create(c.journal_path, 1, journal_bytes)) {
                std::fprintf(stderr, "ttt: cannot create %s\n", c.journal_path.c_str());
                return 1;
            }
            rc = direct(j);
        }
    }
    return rc;
}
