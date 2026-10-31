// The operator's commands to the program (note 004, D5): kill (the kill
// switch: no new order or replace, every open order cancelled), reenable, or
// stop (the end of the day, D8). Waits up to 2 s for the program to take it.
// Design: docs/design/004-program.md
//
// Usage: ttt-ctl kill|reenable|stop [block, default /ttt]
#include <ttt/block.hpp>

#include <cstdio>
#include <string>

#include <unistd.h>

int main(int argc, char** argv) {
    namespace mon = ttt::mon;
    const std::string what = argc > 1 ? argv[1] : "";
    const mon::command c = what == "kill" ? mon::kill : what == "reenable" ? mon::reenable : what == "stop" ? mon::stop : mon::no_command;
    if (c == mon::no_command) {
        std::fprintf(stderr, "usage: ttt-ctl kill|reenable|stop [block]\n");
        return 2;
    }
    mon::block b;
    std::string why;
    if (!b.attach(argc > 2 ? argv[2] : "/ttt", why)) {
        std::fprintf(stderr, "ttt-ctl: %s\n", why.c_str());
        return 1;
    }
    const std::uint64_t n = b.send(c);
    mon::values v{};
    for (int i = 0; i < 200; ++i) {
        b.read(v);
        if (v[mon::command_taken] >= n) {
            std::printf("ttt-ctl: %s (command %llu) taken; kill switch %s\n", what.c_str(), static_cast<unsigned long long>(n),
                        v[mon::killed] ? "engaged" : "off");
            return 0;
        }
        ::usleep(10'000);
    }
    std::fprintf(stderr, "ttt-ctl: %s (command %llu) not taken within 2 s\n", what.c_str(), static_cast<unsigned long long>(n));
    return 1;
}
