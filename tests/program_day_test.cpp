// The sample day through the programs (note 004, S6 and S1), label `data`:
// needs data/01302020.NASDAQ_ITCH50.gz. feed_sender (CPU 5) sends the whole
// day over loopback multicast at 50,000 packets a second, once for each of
// note 001's arrangements: A and B the program `ttt` alone (feed CPU 2,
// strategy CPU 3, engine CPU 4), C `ttt` as a consumer (CPU 3) of `itchfh`'s
// publisher (CPU 2), started before the sender (open item T1). The venue is
// `order_venue` (CPU 6), journal C on /dev/shm. Checked for each: the
// program runs to the end of the day and ends as D8 says (every order
// answered, logged out, exit 0), no answer unmapped; S1: every D, F and G in
// the gateway's journal is a request the risk passed, in the same order, and
// the passed requests not in it are the ones the gateway refused. And note
// 001's S3 as amended (note 004, D7): the runs' orders compared, each against
// the first (its body without TransactTime), up to the first difference.
// Design: docs/design/004-program.md
//
// Usage: program_day_test [day.gz] [arrangements, default ABC] [packets a second, default 50000] [last message, default 0: the day]
// (a last message short of the day is for trials: S6 is the whole day)
#include "check.hpp"
#include "programs.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace ttt::test;

// The number just before `suffix` in the program's output (all ones if none).
std::uint64_t field(const std::string& text, const std::string& suffix) {
    const std::size_t at = text.find(suffix);
    if (at == std::string::npos || at == 0) return ~std::uint64_t{0};
    std::size_t from = at;
    while (from > 0 && text[from - 1] >= '0' && text[from - 1] <= '9') --from;
    return from == at ? ~std::uint64_t{0} : std::stoull(text.substr(from, at - from));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string day = argc > 1 ? argv[1] : TTT_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::string arrangements = argc > 2 ? argv[2] : "ABC";
    const double pace = argc > 3 ? std::atof(argv[3]) : 50'000;
    const std::uint64_t last = argc > 4 ? std::stoull(argv[4]) : 0;
    const std::string tag = std::to_string(::getpid());
    const std::string dir = std::string(TTT_BUILD_DIR) + "/program-day-" + tag;
    std::filesystem::create_directories(dir);
    const std::string pub = "/ttt-pd-" + tag + "-pub", feed_mon = "/ttt-pd-" + tag + "-feed", block = "/ttt-pd-" + tag;
    const std::string feed = dir + "/feed.conf";
    std::ofstream(feed) << "# program_day_test\nport = 26900\nserver_port = 26901\nsnapshot_port = 0\ncpu = 2\nrebuild_cpu = 1\npublish = " << pub
                        << "\nmonitor = " << feed_mon << "\nrecord_dir = none\n";
    const pid_t sender = spawn({TTT_SENDER, "127.0.0.1", "239.192.0.1", "26900", "26901", "5", "0", day, "-"}, dir + "/sender.txt");
    sender_control ctl(26902);
    const bool sender_up = ctl.up(10);
    check(sender_up, "feed_sender loaded the day");
    int k = 0;
    std::vector<std::vector<std::uint64_t>> decided;  // each run's orders, for S3
    for (const char a : arrangements) {
        if (!sender_up) break;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string journal = "/dev/shm/ttt-pd-" + tag + "-" + a + ".journal", passed = dir + "/passed-" + a + ".txt";
        const std::string conf = dir + "/ttt-" + a + ".conf", out = dir + "/ttt-" + a + ".txt";
        const int port = 26910 + k++;
        std::filesystem::remove(journal);
        std::ofstream(conf) << "arrangement = " << a << "\nfeed = feed.conf\nstrategy_cpu = 3\nengine_cpu = 4\nvenue_port = " << port
                            << "\njournal_path = " << journal << "\njournal_mb = 16384\ninstruments = " TTT_CONFIG_DIR "/open-100.txt\nlimits = " TTT_CONFIG_DIR
                            << "/limits.conf\nblock = " << block << "\npassed_log = " << passed << "\n";
        const pid_t venue = spawn({TTT_VENUE, std::to_string(port), "6", "1", dir}, dir + "/venue-" + a + ".txt");
        pid_t itchfh = -1;
        if (a == 'C') {
            itchfh = spawn({TTT_ITCHFH, feed}, dir + "/itchfh.txt");
            for (int i = 0; i < 600 && !std::filesystem::exists("/dev/shm" + pub); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const pid_t program = spawn({TTT_PROGRAM, conf}, out);
        bool attached = false;
        ttt::mon::block b;
        std::string why;
        for (int i = 0; i < 600 && !(attached = b.attach(block, why)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Logged on and the feed ready (its books built; C: the consumer joined) before the day starts.
        ttt::mon::values v{};
        for (int i = 0; attached && i < 1200 && !(v[ttt::mon::logged_on] && v[ttt::mon::feed_ready]); ++i) {
            b.read(v);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        char session[11] = "TTTDAY000A";
        session[9] = a;
        ctl.run(session, pace, last);
        const int exit_code = wait_exit_for(program, 30 * 60 * 1000);
        const int itchfh_exit = itchfh > 0 ? wait_exit_for(itchfh, 60 * 1000) : 0;
        const int venue_exit = wait_exit_for(venue, 60 * 1000);
        itchfh::sender_counts sc{};
        ctl.stop(sc);
        const std::string text = read_file(out), feed_text = a == 'C' ? read_file(dir + "/itchfh.txt") : text;
        std::vector<sent_order> orders;
        const bool read = journal_orders(journal, orders);
        const std::vector<std::uint64_t> ids = read_ids(passed);
        // S1: the journal's orders a subsequence of the passed requests, the rest refused by the gateway.
        std::size_t j = 0;
        for (std::size_t i = 0; i < ids.size() && j < orders.size(); ++i)
            if (ids[i] == orders[j].id) ++j;
        const std::uint64_t gw_refused = field(text, " refused by the gateway"), unmapped = field(text, " unmapped");
        const bool s1 = read && j == orders.size() && ids.size() - orders.size() == gw_refused && text.find("lost ") == std::string::npos;
        const bool ended = exit_code == 0 && itchfh_exit == 0 && venue_exit == 0 && feed_text.find(a == 'C' ? "session ended" : "feed ended") != std::string::npos &&
                           text.find("every order answered; logged out") != std::string::npos;
        const double minutes = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60;
        std::string summary = text.substr(text.rfind("ttt: arrangement"));
        if (!summary.empty() && summary.back() == '\n') summary.pop_back();
        check(ended && unmapped == 0, std::string("S6, arrangement ") + a + ": " + (last ? "messages 1 to " + std::to_string(last) : std::string("the day")) + " at " + std::to_string(static_cast<int>(pace)) +
                                          " packets a second (" + std::to_string(sc.packets) + " sent; " + std::to_string(field(feed_text, " kernel drops")) + " kernel drops) ran to its end and ended as D8 says (exit " +
                                          std::to_string(exit_code) + (a == 'C' ? ", itchfh " + std::to_string(itchfh_exit) : "") + ", venue " +
                                          std::to_string(venue_exit) + "), no answer unmapped; " + std::to_string(minutes).substr(0, 4) +
                                          " min\n      " + summary);
        check(s1, std::string("S1, arrangement ") + a + ": the gateway's journal holds " + std::to_string(orders.size()) +
                      " orders (D, F, G), each a request the risk passed, in its order (" + std::to_string(j) + " matched); " +
                      std::to_string(ids.size()) + " passed, " + std::to_string(ids.size() - orders.size()) + " not sent, " +
                      std::to_string(gw_refused) + " refused by the gateway");
        std::vector<std::uint64_t> d;
        d.reserve(orders.size());
        for (const sent_order& o : orders) d.push_back(o.digest);
        decided.push_back(std::move(d));
        std::filesystem::remove(journal);
    }
    // S3 as amended: agreement published, not required.
    for (std::size_t r = 1; r < decided.size(); ++r) {
        const auto& x = decided[0];
        const auto& y = decided[r];
        const std::size_t n = std::min(x.size(), y.size());
        std::size_t same = 0;
        while (same < n && x[same] == y[same]) ++same;
        std::printf("info  S3 (note 001, amended): arrangement %c's orders against %c's: %zu and %zu orders, the first %zu the same%s\n",
                    arrangements[r], arrangements[0], y.size(), x.size(), same, same == n && x.size() == y.size() ? " (all)" : "");
    }
    ctl.quit();
    wait_exit_for(sender, 60 * 1000);
    if (ttt::test::failures == 0) std::filesystem::remove_all(dir);
    return ttt::test::result();
}
