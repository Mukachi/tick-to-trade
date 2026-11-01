// Tests for include/ttt/config.hpp (note 004): the example file is read,
// paths from its directory; every key known, a bad value or a key given twice
// refused with its line; the feed handler's file required and checked.
// Design: docs/design/004-program.md
#include "check.hpp"

#include <ttt/config.hpp>

#include <filesystem>
#include <string>

namespace {

using ttt::test::check;

bool parses(const std::string& text, std::string& why, ttt::program_config& c) {
    return ttt::parse_program(text, TTT_CONFIG_DIR, c, why);
}

}  // namespace

int main() {
    ttt::program_config c;
    std::string why;
    check(ttt::load_program(TTT_CONFIG_DIR "/ttt-loopback.conf", c, why) && c.arrangement == 'A' && c.venue_port == 26500 &&
              c.feed_config.port == 26400 && c.feed_config.snapshot_port == 26403 &&
              c.instruments == std::string(TTT_CONFIG_DIR) + "/open-100.txt" && c.sender_comp_id == "CLIENT",
          "config/ttt-loopback.conf read, its feed file with it, paths from its directory " + why);
    const std::string feed = "feed = feed-loopback.conf\n";
    {
        ttt::program_config x;
        std::string w;
        check(c.store_mb == 2048 && c.store_messages == 16'000'000 && parses(feed + "store_mb = 4096\nstore_messages = 32000000\n", w, x) &&
                  x.store_mb == 4096 && x.store_messages == 32'000'000,
              "the gateway's store reserved for the day by default (2 GB, 16M messages), and as given " + w);
    }
    const struct {
        std::string text, expect;
    } bad[] = {
        {feed + "arrangment = A\n", "line 2: unknown key arrangment"},
        {feed + "arrangement = D\n", "line 2: arrangement: A, B or C"},
        {feed + "venue_port = 26500\nvenue_port = 26501\n", "line 3: venue_port given twice"},
        {feed + "engine_cpu = x\n", "line 2: engine_cpu: a CPU number"},
        {feed + "journal = B\n", "line 2: journal: A or C"},
        {feed + "block = ttt\n", "line 2: block: a shared-memory name (/name)"},
        {feed + "store_mb = 0\n", "line 2: store_mb: 1 to 65536"},
        {"arrangement = A\n", "feed: the feed handler's configuration file is required"},
        {"feed = no-such.conf\n", "no-such.conf: cannot read"},
    };
    for (const auto& b : bad) {
        ttt::program_config x;
        std::string w;
        const bool refused = !parses(b.text, w, x) && w.find(b.expect) != std::string::npos;
        check(refused, "refused: " + w);
    }
    return ttt::test::result();
}
