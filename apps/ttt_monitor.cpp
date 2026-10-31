// Prints the program's counters (note 004): once, or every second.
// Design: docs/design/004-program.md
//
// Usage: ttt-monitor [block, default /ttt] [--follow]
#include <ttt/block.hpp>

#include <cstdio>
#include <string>

#include <unistd.h>

int main(int argc, char** argv) {
    namespace mon = ttt::mon;
    std::string name = "/ttt";
    bool follow = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--follow") follow = true;
        else name = a;
    }
    mon::block b;
    std::string why;
    if (!b.attach(name, why)) {
        std::fprintf(stderr, "ttt-monitor: %s\n", why.c_str());
        return 1;
    }
    do {
        mon::values v{};
        b.read(v);
        std::printf("%s (pid %d)\n", name.c_str(), b.pid());
        for (std::uint32_t i = 0; i < mon::n_counters; ++i) {
            if (i == mon::arrangement) std::printf("  %-28s %c\n", mon::counter_name(i), static_cast<char>(v[i]));
            else if (i == mon::last_command) std::printf("  %-28s %s\n", mon::counter_name(i), mon::command_name(static_cast<std::uint8_t>(v[i])));
            else std::printf("  %-28s %llu\n", mon::counter_name(i), static_cast<unsigned long long>(v[i]));
        }
        std::fflush(stdout);
        if (follow) ::sleep(1);
    } while (follow);
    return 0;
}
