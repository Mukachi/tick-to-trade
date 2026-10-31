// The risk checks' limits (note 003), and their configuration file: one
// `key = value` a line, `#` to the end of a line a comment, every key known.
// Prices in four implied decimals (ITCH's), values in 1e-4 dollars (price x
// shares), bands in basis points, windows in nanoseconds of market time
// (D1: the ITCH timestamp).
// Design: docs/design/003-pre-trade-risk.md
#pragma once

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ttt {

struct limits {
    std::uint32_t max_volume = 10'000;                    // Art. 15(1)(c): shares an order
    std::uint64_t max_value = 1'000'000'000;              // Art. 15(1)(b): $100,000 an order
    std::uint32_t collar_bps = 500;                       // Art. 15(1)(a): from the best ask (and below the best bid)
    std::uint32_t period_collar_bps = 1'000;              // D2: from the period's reference price
    std::uint64_t period_ns = 60'000'000'000;             // D2: one minute
    std::uint32_t max_messages = 1'000;                   // Art. 15(1)(d): news, replaces, cancels ...
    std::uint64_t messages_window_ns = 1'000'000'000;     // ... a second
    std::uint32_t max_fills = 1'000;                      // D4, Art. 15(3): fills ...
    std::uint64_t fills_window_ns = 60'000'000'000;       // ... a minute, then the kill switch
    std::uint64_t instrument_exposure = 10'000'000'000;   // D5: $1 million an instrument
    std::uint64_t firm_exposure = 100'000'000'000;        // D5: $10 million for the firm
    std::vector<std::string> permitted;                   // Art. 15(5): symbols; empty: every traded instrument
};

// Reads `path` over the defaults; false, with `why`, on an unknown key or a bad value.
inline bool read_limits(const std::string& path, limits& l, std::string& why) {
    std::ifstream f(path);
    if (!f) {
        why = path + ": cannot read";
        return false;
    }
    int n = 0;
    for (std::string line; std::getline(f, line);) {
        ++n;
        if (const auto h = line.find('#'); h != std::string::npos) line.resize(h);
        const auto eq = line.find('=');
        const auto trim = [](std::string s) {
            const auto a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
        };
        if (trim(line).empty()) continue;
        const std::string where = path + ":" + std::to_string(n) + ": ";
        if (eq == std::string::npos) {
            why = where + "not key = value";
            return false;
        }
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "permitted") {
            l.permitted.clear();
            std::stringstream ss(v);
            for (std::string s; std::getline(ss, s, ',');)
                if (!trim(s).empty()) l.permitted.push_back(trim(s));
            continue;
        }
        std::uint64_t x = 0;
        std::size_t used = 0;
        try {
            x = std::stoull(v, &used);
        } catch (...) {
            used = 0;
        }
        if (used == 0 || used != v.size()) {
            why = where + k + ": not a whole number";
            return false;
        }
        const auto u32 = [&](std::uint32_t& f) {
            if (x > 0xFFFF'FFFFull) return false;
            f = static_cast<std::uint32_t>(x);
            return true;
        };
        bool ok = true;
        if (k == "max_volume") ok = u32(l.max_volume);
        else if (k == "max_value") l.max_value = x;
        else if (k == "collar_bps") ok = u32(l.collar_bps);
        else if (k == "period_collar_bps") ok = u32(l.period_collar_bps);
        else if (k == "period_ns") l.period_ns = x;
        else if (k == "max_messages") ok = u32(l.max_messages);
        else if (k == "messages_window_ns") l.messages_window_ns = x;
        else if (k == "max_fills") ok = u32(l.max_fills);
        else if (k == "fills_window_ns") l.fills_window_ns = x;
        else if (k == "instrument_exposure") l.instrument_exposure = x;
        else if (k == "firm_exposure") l.firm_exposure = x;
        else {
            why = where + k + ": unknown key";
            return false;
        }
        if (!ok || x == 0) {
            why = where + k + ": out of range";
            return false;
        }
    }
    return true;
}

}  // namespace ttt
