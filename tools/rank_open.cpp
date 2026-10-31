// The instrument lists for the benchmarks (note 002, D5): every instrument of
// the sample day ranked by its best bid's price changes over the open
// (09:29:30 for five minutes), the most first, through the feed handler's
// handler on its default book. Writes the 1, 10 and 100 at the top, a symbol a
// line, to <dir>/open-<n>.txt, and prints the top 100 with their counts.
//
// Usage: rank_open [day.gz] [dir]
#include "day_file.hpp"

#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace itchfh;

struct counting {
    const default_book* b = nullptr;
    std::uint64_t now = 0;
    std::vector<std::uint32_t> last_bid = std::vector<std::uint32_t>(65'536, 0);
    std::vector<std::uint64_t> changes = std::vector<std::uint64_t>(65'536, 0);
    std::vector<itchfh::symbol> names = std::vector<itchfh::symbol>(65'536);
    void book_changed(std::uint16_t l) {
        const auto best = b->best(l, side::bid);
        const std::uint32_t p = best ? best->price : 0;
        if (p == last_bid[l]) return;
        last_bid[l] = p;
        if (now >= ttt::open_from_ns && now < ttt::open_to_ns) ++changes[l];
    }
    void book(const applied& a) {
        if (a.bad == bad_event::none) book_changed(a.locate);
    }
    void trade(const trade_report&) {}
    void trade_broken(const trade_report&) {}
    void status(std::uint16_t, status_field) {}
    void imbalance(std::uint16_t, const itchfh::imbalance&) {}
    void market(const message&) {}
    void directory(std::uint16_t l, const stock_directory& r) { names[l] = r.stock; }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : TTT_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::string dir = argc > 2 ? argv[2] : TTT_CONFIG_DIR;
    ttt::day_file day(path);
    if (!day.open()) {
        std::fprintf(stderr, "%s: cannot read\n", path.c_str());
        return 1;
    }
    auto book = std::make_unique<default_book>();
    auto c = std::make_unique<counting>();
    c->b = book.get();
    auto h = std::make_unique<handler<default_book, counting>>(*book, *c);
    message m;
    while (day.next(m)) {
        if (m.timestamp_ns >= ttt::open_to_ns) break;  // nothing after the open counts
        c->now = m.timestamp_ns;
        h->on(m);
    }
    std::vector<std::uint16_t> order;
    for (std::uint32_t l = 0; l < 65'536; ++l)
        if (c->changes[l]) order.push_back(static_cast<std::uint16_t>(l));
    // The most changes first; ties by locate, so the lists never depend on the sort.
    std::sort(order.begin(), order.end(), [&](std::uint16_t a, std::uint16_t b) {
        return c->changes[a] != c->changes[b] ? c->changes[a] > c->changes[b] : a < b;
    });
    const auto name = [&](std::uint16_t l) {
        std::string s(c->names[l].data(), c->names[l].size());
        return s.substr(0, s.find_last_not_of(' ') + 1);
    };
    std::printf("rank locate symbol changes (best bid price, %s)\n", "09:29:30 for five minutes");
    for (std::size_t i = 0; i < order.size() && i < 100; ++i) std::printf("%4zu %6u %-8s %llu\n", i + 1, order[i], name(order[i]).c_str(), static_cast<unsigned long long>(c->changes[order[i]]));
    for (const std::size_t n : {1, 10, 100}) {
        std::ofstream f(dir + "/open-" + std::to_string(n) + ".txt");
        for (std::size_t i = 0; i < n && i < order.size(); ++i) f << name(order[i]) << '\n';
        if (!f) {
            std::fprintf(stderr, "%s: cannot write\n", dir.c_str());
            return 1;
        }
    }
    std::printf("%zu instruments changed their best bid over the open; lists written to %s\n", order.size(), dir.c_str());
    return 0;
}
