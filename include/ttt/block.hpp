// The program's block of shared memory (note 004, D5): the operator's
// commands in, the monitoring counters out.
//   commands   one word, (number << 8) | command, swapped in by `ttt-ctl`
//              (compare and swap, so two at once cannot lose one); the
//              strategy thread reads it each pass and takes a new number once,
//              acknowledging it in the counters (`command_taken`)
//   counters   copied by the strategy thread every 100 ms under the feed
//              handler's seqlock (its note 009: release stores and acquire
//              loads, word by word), no allocation or system call; read by
//              `ttt-monitor`
// Design: docs/design/004-program.md
#pragma once

#include <ttt/risk.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ttt::mon {

enum command : std::uint8_t { no_command = 0, kill = 1, reenable = 2, stop = 3 };
inline const char* command_name(std::uint8_t c) {
    return c == kill ? "kill" : c == reenable ? "reenable" : c == stop ? "stop" : "none";
}

enum counter : std::uint32_t {
    arrangement, feed_ready, logged_on, killed, feed_ended, ending,
    events, answers, unmapped, unknown_strategy, news, replaces, cancels,
    passed, risk_cancels, fills, kills, kill_cancels, unknown_risk, firm_exposure, open_orders,
    refused_total, refused_first,  // then one a reason after `none`: refused_first + (reason - 1)
    gateway_refused = refused_first + refusal_kinds - 1, app_ring_full, ring_set_aside, ring_replaced,
    command_taken, last_command, refreshes,
    n_counters
};
inline const char* counter_name(std::uint32_t i) {
    static const char* const head[] = {"arrangement", "feed_ready", "logged_on", "killed", "feed_ended", "ending", "events", "answers", "unmapped",
                                       "unknown_strategy", "news", "replaces", "cancels", "passed", "risk_cancels", "fills", "kills",
                                       "kill_cancels", "unknown_risk", "firm_exposure", "open_orders", "refused"};
    static const char* const tail[] = {"gateway_refused", "app_ring_full", "ring_set_aside", "ring_replaced", "command_taken", "last_command",
                                       "refreshes"};
    static thread_local std::string by_reason;
    if (i < refused_first) return head[i];
    if (i < gateway_refused) {
        by_reason = std::string("refused_") + name(static_cast<refusal>(i - refused_first + 1));
        return by_reason.c_str();
    }
    return tail[i - gateway_refused];
}

using values = std::uint64_t[n_counters];

inline constexpr std::uint64_t block_magic = 0x7474746d6f6e3031ULL;  // "tttmon01"
inline constexpr std::uint32_t block_version = 1;

struct alignas(64) block_layout {
    std::uint64_t magic;
    std::uint32_t version, counters;
    std::atomic<std::uint32_t> ready;
    std::int32_t pid;
    alignas(64) std::atomic<std::uint64_t> command{0};  // (number << 8) | command
    alignas(64) std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> v[n_counters]{};
};

class block {
public:
    block() = default;
    block(const block&) = delete;
    block& operator=(const block&) = delete;
    ~block() {
        if (b_) ::munmap(b_, sizeof(block_layout));
        if (creator_) ::shm_unlink(name_.c_str());
    }
    // Replaces a block whose creator has gone; refuses a live one.
    bool create(const std::string& name, std::string& why) {
        name_ = name;
        int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0 && errno == EEXIST) {
            block old;
            if (old.attach(name, why) && old.b_->pid > 0 && (::kill(old.b_->pid, 0) == 0 || errno == EPERM))
                return fail(why, name + ": in use by a running program");
            ::shm_unlink(name.c_str());
            fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        }
        if (fd < 0) return fail(why, name + ": shm_open: " + std::strerror(errno));
        if (::ftruncate(fd, sizeof(block_layout)) != 0) {
            ::close(fd);
            return fail(why, name + ": ftruncate: " + std::strerror(errno));
        }
        void* m = ::mmap(nullptr, sizeof(block_layout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) return fail(why, name + ": mmap: " + std::strerror(errno));
        creator_ = true;
        b_ = new (m) block_layout{};
        b_->magic = block_magic;
        b_->version = block_version;
        b_->counters = n_counters;
        b_->pid = static_cast<std::int32_t>(::getpid());
        b_->ready.store(1, std::memory_order_release);
        return true;
    }
    bool attach(const std::string& name, std::string& why) {
        name_ = name;
        const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) return fail(why, name + ": shm_open: " + std::strerror(errno));
        struct stat st {};
        if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) != sizeof(block_layout)) {
            ::close(fd);
            return fail(why, name + ": not a block of this layout");
        }
        void* m = ::mmap(nullptr, sizeof(block_layout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) return fail(why, name + ": mmap: " + std::strerror(errno));
        b_ = std::launder(static_cast<block_layout*>(m));
        if (b_->ready.load(std::memory_order_acquire) != 1 || b_->magic != block_magic || b_->version != block_version ||
            b_->counters != n_counters)
            return fail(why, name + ": not a ready block of this version");
        return true;
    }

    // The writer's (the strategy thread). No allocation, no system call.
    void write(const values& v) {
        const std::uint64_t s = b_->seq.load(std::memory_order_relaxed);
        b_->seq.store(s + 1, std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < n_counters; ++i) b_->v[i].store(v[i], std::memory_order_release);
        b_->seq.store(s + 2, std::memory_order_release);
    }
    // A reader's: retries until it has a consistent copy.
    void read(values& v) const {
        for (;;) {
            const std::uint64_t s1 = b_->seq.load(std::memory_order_acquire);
            if (s1 & 1) continue;
            for (std::uint32_t i = 0; i < n_counters; ++i) v[i] = b_->v[i].load(std::memory_order_acquire);
            if (b_->seq.load(std::memory_order_relaxed) == s1) return;
        }
    }
    // The operator's: a command, numbered after the last; returns its number.
    std::uint64_t send(command c) {
        std::uint64_t w = b_->command.load(std::memory_order_acquire);
        while (!b_->command.compare_exchange_weak(w, (((w >> 8) + 1) << 8) | c, std::memory_order_acq_rel)) {
        }
        return (w >> 8) + 1;
    }
    // The program's: the command word as it stands.
    std::uint64_t command_word() const { return b_->command.load(std::memory_order_acquire); }
    std::int32_t pid() const { return b_->pid; }

private:
    static bool fail(std::string& why, std::string w) {
        why = std::move(w);
        return false;
    }
    std::string name_;
    block_layout* b_ = nullptr;
    bool creator_ = false;
};

}  // namespace ttt::mon
