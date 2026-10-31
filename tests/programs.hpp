// Helpers for the tests through the programs (note 004, S1, S2, S6): starting
// and waiting for processes, the feed handler's sender over its control port
// (its bench/feed_sender.cpp), and the gateway's journal read back.
#pragma once

#include <ttt/block.hpp>

#include <itchfh/mold_sender.hpp>

#include <fixgw/journal.hpp>
#include <fixgw/orders.hpp>

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace ttt::test {

inline pid_t spawn(const std::vector<std::string>& args, const std::string& out = "") {
    std::fflush(stdout);  // or the child's freopen writes what is buffered a second time
    std::fflush(stderr);
    const pid_t p = ::fork();
    if (p == 0) {
        if (!out.empty()) {
            std::FILE* f = std::freopen(out.c_str(), "w", stdout);
            if (f) ::dup2(::fileno(stdout), 2);
        }
        std::vector<char*> a;
        for (const auto& s : args) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        ::execv(a[0], a.data());
        ::_exit(127);
    }
    return p;
}
inline int wait_exit(pid_t p) {
    int st = 0;
    ::waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
// The exit code, or -2 if it has not exited within `ms` (then killed).
inline int wait_exit_for(pid_t p, int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        int st = 0;
        if (::waitpid(p, &st, WNOHANG) == p) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(p, SIGKILL);
    wait_exit(p);
    return -2;
}

// feed_sender's control port.
struct sender_control {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in to{};
    explicit sender_control(std::uint16_t port) {
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        timeval tv{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~sender_control() { ::close(fd); }
    bool stop(itchfh::sender_counts& c) {
        const unsigned char s = 'S';
        ::sendto(fd, &s, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        return ::recv(fd, &c, sizeof(c), 0) == static_cast<ssize_t>(sizeof(c));
    }
    // Waits for the sender to have loaded the day (up to `minutes`).
    bool up(int minutes) {
        itchfh::sender_counts c;
        for (int i = 0; i < minutes * 60; ++i)
            if (stop(c)) return true;
        return false;
    }
    // Messages 1..last (0: the whole day) at a fixed pace, from..to lost on purpose (0: none).
    void run(const char (&session)[11], double packets_per_s, std::uint64_t last, std::uint64_t lose_from = 0, std::uint64_t lose_to = 0) {
        unsigned char b[75] = {};
        const double zero = 0;
        b[0] = 'R';
        std::memcpy(b + 1, session, 10);
        std::memcpy(b + 11, &packets_per_s, 8);
        std::memcpy(b + 19, &zero, 8);
        std::memcpy(b + 27, &last, 8);
        std::memcpy(b + 59, &lose_from, 8);
        std::memcpy(b + 67, &lose_to, 8);
        ::sendto(fd, b, sizeof(b), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        std::uint64_t span[2];
        [[maybe_unused]] const ssize_t n = ::recv(fd, span, sizeof(span), 0);
    }
    void quit() {
        const unsigned char q = 'Q';
        ::sendto(fd, &q, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
};

// An order message the gateway journaled as sent (its note 004: a send
// record, the MsgType's length and MsgType, then the body).
struct sent_order {
    char type;
    std::uint64_t id;
    std::int64_t mono_ns;
};
inline bool journal_orders(const std::string& path, std::vector<sent_order>& out) {
    fixgw::journal_reader r;
    if (!r.open(path)) return false;
    for (const fixgw::journal_record& rec : r.records()) {
        if (rec.kind != fixgw::record_kind::send || rec.payload.size() < 2) continue;
        const std::size_t tl = static_cast<unsigned char>(rec.payload[0]);
        if (tl != 1) continue;
        const char t = rec.payload[1];
        if (t != 'D' && t != 'F' && t != 'G') continue;
        const auto id = fixgw::detail::body_field(rec.payload.substr(2), 11);
        if (!id) return false;
        out.push_back({t, std::stoull(std::string(*id)), rec.time.mono_ns});
    }
    return true;
}
inline std::vector<std::uint64_t> read_ids(const std::string& path) {
    std::vector<std::uint64_t> v;
    std::ifstream f(path);
    for (std::string s; std::getline(f, s);)
        if (!s.empty() && s[0] >= '0' && s[0] <= '9') v.push_back(std::stoull(s));
    return v;
}
inline std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), {}};
}

}  // namespace ttt::test
