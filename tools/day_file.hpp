// The sample day's messages, one at a time: Nasdaq's file (gzip, each message
// after a two-byte length) read through gzip, decoded with the feed handler's
// default decoder. For the tools and the tests over the day (label `data`).
#pragma once

#include <itchfh/decoder.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ttt {

class day_file {
public:
    explicit day_file(const std::string& path) : in_(::popen(("gzip -dc '" + path + "'").c_str(), "r")), buf_(1 << 16) {}
    ~day_file() {
        if (in_) ::pclose(in_);
    }
    day_file(const day_file&) = delete;
    day_file& operator=(const day_file&) = delete;

    bool open() const { return in_ != nullptr; }
    // The next message (its sequence number is its place in the day, from 1); false at the end.
    bool next(itchfh::message& m) {
        for (;;) {
            unsigned char lb[2];
            if (std::fread(lb, 1, 2, in_) != 2) return false;
            const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
            if (len == 0 || std::fread(buf_.data(), 1, len, in_) != len) return false;
            ++sequence_;
            if (itchfh::decode(buf_.data(), len, m) == itchfh::decode_status::ok) return true;
            ++undecoded_;
        }
    }
    std::uint64_t sequence() const { return sequence_; }
    std::uint64_t undecoded() const { return undecoded_; }

private:
    std::FILE* in_;
    std::vector<unsigned char> buf_;
    std::uint64_t sequence_ = 0, undecoded_ = 0;
};

// The open (note 001, D7): 09:29:30 for five minutes, in nanoseconds since midnight.
inline constexpr std::uint64_t open_from_ns = (9 * 3600 + 29 * 60 + 30) * 1'000'000'000ull;
inline constexpr std::uint64_t open_to_ns = open_from_ns + 300 * 1'000'000'000ull;

}  // namespace ttt
