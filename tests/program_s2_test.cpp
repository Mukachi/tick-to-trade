// Note 004's S2 through the programs (label `data`: needs data/), arrangement
// A as program_day_test runs it, the sender at 15,000 packets a second over
// the day's first 40 million messages, its snapshot service on CPU 7 (the
// feed handler's note 011), messages 25,000,000 to 25,001,999 lost on purpose:
//   kill      once orders flow, `ttt-ctl kill`: every order answered as gone
//             within 2 s, and no D or G in the gateway's journal from then to
//             `ttt-ctl reenable`; after it, orders resume
//   venue     the venue's connection dropped and refused for 2 s: the
//             program sees the session down, and no D or G is journaled from
//             then until the venue takes connections again; after it, the
//             session logs on again, catches up (the venue's own cancels of
//             our orders, cancel-on-disconnect) and orders resume. The venue
//             is the gateway's session and venue simulator on a thread here,
//             as order_venue but keeping its session across connections: a
//             restarted order_venue is a new session (sequence numbers from
//             1), which the gateway rightly refuses (MsgSeqNum too low)
//   feed      the loss recovered from a snapshot with the strategy's sink in
//             place of the publisher (open item T2): one snapshot, none
//             failed; no D or G journaled while the feed was seen lost
// and the run ends as D8 says. Times are CLOCK_MONOTONIC, the gateway's
// journal's clock; each window is taken inside what was seen, so it is never
// wider than the truth.
// Design: docs/design/004-program.md
//
// Usage: program_s2_test [day.gz]
#include "check.hpp"
#include "programs.hpp"

#include <itchfh/monitoring.hpp>

#include <fixgw/engine.hpp>  // clock_now
#include <fixgw/session.hpp>
#include <fixgw/transport.hpp>
#include <fixgw/venue_sim.hpp>

#include <lfipc/bench/cross_cpu.hpp>

#include <atomic>
#include <map>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace ttt::test;
namespace mon = ttt::mon;

std::int64_t mono() {
    timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1'000'000'000LL + t.tv_nsec;
}

// The venue: order_venue's session and simulator, one session for the
// test's life; `drop` closes the connection, `down` refuses new ones.
struct test_venue {
    std::atomic<bool> drop{false}, down{false}, quit{false};
    std::atomic<int> connections{0};
    std::thread t;
    struct store {
        std::map<std::uint64_t, std::string> m;
        void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
        std::string_view lookup(std::uint64_t s) {
            const auto it = m.find(s);
            return it == m.end() ? std::string_view{} : std::string_view(it->second);
        }
        void reset() { m.clear(); }
    };
    struct sink {
        fixgw::tcp_connection* c = nullptr;
        fixgw::venue_sim* sim = nullptr;
        fixgw::session<sink, store>* self = nullptr;
        std::string out;
        void write(std::string_view b) { out += b; }
        void send() {
            if (c && c->open() && !out.empty()) c->write(out), c->flush();
            out.clear();
        }
        void disconnect() {
            send();
            if (c) c->close();
        }
        void deliver(const fixgw::message_view<>& m) {
            sim->on_message(1, m, [&](int s, std::string_view ty, const std::string& b) {
                if (s == 1) self->send(ty, b, fixgw::clock_now());
            });
        }
        void log(fixgw::session_log, std::string_view, std::uint64_t) {}
        bool resend(const fixgw::message_view<>&) { return true; }
        bool release(std::string_view, std::string_view) { return true; }
    };
    void start(std::uint16_t port, int cpu) {
        t = std::thread([this, port, cpu] {
            lfipc::bench::detail::pin_to(cpu);
            fixgw::tcp_listener l;
            l.open(port);
            fixgw::session_config cfg;
            cfg.role = fixgw::session_role::acceptor;
            cfg.sender_comp_id = "VENUE";
            cfg.target_comp_id = "CLIENT";
            cfg.app_msg_types = "D F G";
            fixgw::venue_config vc;
            vc.capacity = 1u << 20;
            fixgw::venue_sim sim(vc);
            store st;
            sink sk;
            sk.sim = &sim;
            fixgw::session<sink, store> s(cfg, sk, st);
            sk.self = &s;
            char b[1 << 16];
            while (!quit.load()) {
                if (down.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                const int fd = l.accept();
                if (fd < 0) continue;
                fixgw::tcp_connection c(64u << 20);
                c.attach(fd);
                sk.c = &c;
                s.on_connected(fixgw::clock_now());
                ++connections;
                std::string inbox;
                while (c.open() && !quit.load()) {
                    const long n = c.read_some(b, sizeof(b));
                    if (n < 0) break;
                    if (n > 0) inbox.append(b, static_cast<std::size_t>(n));
                    if (!inbox.empty()) inbox.erase(0, s.on_bytes(inbox.data(), inbox.size(), fixgw::clock_now()));
                    s.on_tick(fixgw::clock_now());
                    sk.send();
                    if (drop.exchange(false)) break;
                }
                c.close();
                sk.c = nullptr;
                s.on_disconnected(fixgw::clock_now());
                sim.on_disconnect(1, [&](int ss, std::string_view ty, const std::string& body) {
                    if (ss == 1) s.send(ty, body, fixgw::clock_now());  // held until the session is back, then sent
                });
                sk.out.clear();
            }
        });
    }
};

// D and G journaled in [from, to).
std::size_t orders_in(const std::vector<sent_order>& o, std::int64_t from, std::int64_t to) {
    std::size_t n = 0;
    for (const sent_order& s : o) n += (s.type == 'D' || s.type == 'G') && s.mono_ns >= from && s.mono_ns < to;
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string day = argc > 1 ? argv[1] : TTT_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::string tag = std::to_string(::getpid());
    const std::string dir = std::string(TTT_BUILD_DIR) + "/program-s2-" + tag;
    std::filesystem::create_directories(dir);
    const std::string feed_mon = "/ttt-s2-" + tag + "-feed", block = "/ttt-s2-" + tag, journal = "/dev/shm/ttt-s2-" + tag + ".journal";
    std::ofstream(dir + "/feed.conf") << "# program_s2_test\nport = 26950\nserver_port = 26951\nsnapshot_port = 26953\ncpu = 2\nrebuild_cpu = 1\npublish = none"
                                      << "\nmonitor = " << feed_mon << "\nrecord_dir = none\n";
    const std::string conf = dir + "/ttt.conf";
    std::ofstream(conf) << "arrangement = A\nfeed = feed.conf\nstrategy_cpu = 3\nengine_cpu = 4\nvenue_port = 26960\njournal_path = " << journal
                        << "\njournal_mb = 4096\ninstruments = " TTT_CONFIG_DIR "/open-100.txt\nlimits = " TTT_CONFIG_DIR "/limits.conf\nblock = " << block
                        << "\npassed_log = none\n";
    std::filesystem::remove(journal);
    constexpr std::uint64_t last = 40'000'000, lose_from = 25'000'000, lose_to = lose_from + 1999;

    const pid_t sender = spawn({TTT_SENDER, "127.0.0.1", "239.192.0.1", "26950", "26951", "5", "0", day, "-", "7"}, dir + "/sender.txt");
    sender_control ctl(26952);
    if (!ctl.up(10)) {
        check(false, "feed_sender loaded the day");
        return ttt::test::result();
    }
    test_venue venue;
    venue.start(26960, 6);
    const pid_t program = spawn({TTT_PROGRAM, conf}, dir + "/ttt.txt");
    mon::block b;
    itchfh::mon::block fb;
    std::string why;
    bool attached = false;
    for (int i = 0; i < 600 && !(attached = b.attach(block, why)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mon::values v{};
    itchfh::mon::values fv{};
    bool feed_attached = false;
    const auto read = [&] {
        if (attached) b.read(v);
        if (!feed_attached) feed_attached = fb.attach(feed_mon, why);
        if (feed_attached) fb.read(fv);
    };
    // Waits for `cond` (read every 10 ms) up to `ms`; false if it never held.
    const auto wait_for = [&](auto cond, int ms) {
        for (int i = 0; i < ms / 10; ++i) {
            read();
            if (cond()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    const bool ready = wait_for([&] { return v[mon::logged_on] && v[mon::feed_ready]; }, 120'000);
    check(ready, "the program logged on and its feed ready");
    const char session[11] = "TTTS2KILLS";
    ctl.run(session, 15'000, last, lose_from, lose_to);

    // The kill switch.
    const bool flowing = wait_for([&] { return v[mon::passed] > 20'000; }, 60'000);
    const int kill_exit = wait_exit(spawn({TTT_CTL, "kill", block}, dir + "/ctl-kill.txt"));
    const std::int64_t killed_at = mono();
    const bool all_gone = wait_for([&] { return v[mon::killed] && v[mon::open_orders] == 0; }, 2000);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const std::int64_t reenable_at = mono();
    const int reenable_exit = wait_exit(spawn({TTT_CTL, "reenable", block}, dir + "/ctl-reenable.txt"));
    read();
    const std::uint64_t passed_at_reenable = v[mon::passed];
    const bool resumed = wait_for([&] { return !v[mon::killed] && v[mon::passed] > passed_at_reenable + 100; }, 5000);

    // The venue's connection gone, and back.
    venue.down = true;
    venue.drop = true;
    const bool seen_down = wait_for([&] { return v[mon::logged_on] == 0; }, 5000);
    const std::int64_t down_seen_at = mono();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const std::int64_t venue_restarted_at = mono();
    venue.down = false;
    const bool back = wait_for([&] { return v[mon::logged_on] == 1; }, 10'000);
    read();
    const std::uint64_t passed_at_back = v[mon::passed];
    const bool resumed_after_venue = wait_for([&] { return v[mon::passed] > passed_at_back + 100; }, 10'000);

    // The loss at message 25,000,000: the feed seen lost, then live again.
    std::int64_t lost_seen = 0, live_again_seen = 0;
    const bool lost = wait_for([&] { return static_cast<itchfh::feed_state>(fv[itchfh::mon::feed_state_now]) == itchfh::feed_state::lost; }, 120'000);
    if (lost) lost_seen = mono();
    const bool live_again = lost && wait_for([&] { return static_cast<itchfh::feed_state>(fv[itchfh::mon::feed_state_now]) == itchfh::feed_state::live; }, 60'000);
    if (live_again) live_again_seen = mono();
    // The program's last counters, read until it exits (its blocks go with it).
    itchfh::mon::values last_feed{};
    int status = 0;
    const std::int64_t give_up = mono() + 10LL * 60 * 1'000'000'000;
    while (::waitpid(program, &status, WNOHANG) == 0) {
        if (mono() > give_up) {
            ::kill(program, SIGKILL);
            ::waitpid(program, &status, 0);
            break;
        }
        read();
        std::memcpy(last_feed, fv, sizeof(fv));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    venue.quit = true;
    venue.t.join();
    const int venue_exit = venue.connections.load() == 2 ? 0 : 1;
    itchfh::sender_counts sc{};
    ctl.stop(sc);
    ctl.quit();
    wait_exit_for(sender, 60'000);

    const std::string text = read_file(dir + "/ttt.txt");
    std::vector<sent_order> orders;
    const bool journal_read = journal_orders(journal, orders);
    const std::size_t during_kill = orders_in(orders, killed_at, reenable_at);
    const std::size_t while_down = orders_in(orders, down_seen_at, venue_restarted_at);
    const std::size_t while_lost = lost && live_again ? orders_in(orders, lost_seen, live_again_seen) : 0;
    check(flowing && kill_exit == 0 && all_gone && journal_read && during_kill == 0 && reenable_exit == 0 && resumed,
          "S2, the kill switch: ttt-ctl kill taken (exit " + std::to_string(kill_exit) + "), every order answered as gone within 2 s (" +
              (all_gone ? "yes" : "NO") + "); " + std::to_string(during_kill) + " D or G journaled from the kill to ttt-ctl reenable (3 s); orders resumed after it (" +
              (resumed ? "yes" : "NO") + ")");
    check(seen_down && back && while_down == 0 && resumed_after_venue,
          "S2, the session: the venue's connection dropped, the program saw the session down (" + std::string(seen_down ? "yes" : "NO") + "); " +
              std::to_string(while_down) + " D or G journaled from then until the venue took connections again (2 s); logged on again (" + (back ? "yes" : "NO") +
              ") and orders resumed (" + (resumed_after_venue ? "yes" : "NO") + ")");
    check(lost && live_again && last_feed[itchfh::mon::snapshots] == 1 && last_feed[itchfh::mon::snapshot_failures] == 0 && while_lost == 0 &&
              sc.lost_on_purpose > 0,
          "S2, the feed: 2,000 messages lost on purpose (" + std::to_string(sc.lost_on_purpose) + " packets not sent): the feed seen lost (" +
              (lost ? "yes" : "NO") + ") and live again (" + (live_again ? "yes" : "NO") + "), recovered from " +
              std::to_string(last_feed[itchfh::mon::snapshots]) + " snapshot (" + std::to_string(last_feed[itchfh::mon::snapshot_failures]) +
              " failed) through the strategy's sink (T2) in " + std::to_string(last_feed[itchfh::mon::last_recovery_ns] / 1'000'000) + " ms; " +
              std::to_string(while_lost) + " D or G journaled while it was seen lost");
    const bool ended = text.find("feed ended") != std::string::npos && text.find("every order answered; logged out") != std::string::npos && venue_exit == 0;
    std::string summary = text.find("ttt: arrangement A;") != std::string::npos ? text.substr(text.find("ttt: arrangement A;")) : text;
    if (!summary.empty() && summary.back() == '\n') summary.pop_back();
    check(ended && exit_code == 0, "D8: the run ended with every order answered and the session logged out (exit " + std::to_string(exit_code) +
                                       ")\n      " + summary);
    std::filesystem::remove(journal);
    if (ttt::test::failures == 0) std::filesystem::remove_all(dir);
    return ttt::test::result();
}
