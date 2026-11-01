// The program's configuration (note 004): one file of `key = value` lines,
// `#` to the end of a line a comment, checked as the feed handler's (its note
// 009, D7): every key known, a bad value or a key given twice refused with
// its line. Relative paths are from the file's directory. The feed's own keys
// are the feed handler's configuration file, named by `feed`: the same file
// `itchfh` reads in arrangement C.
// Design: docs/design/004-program.md
#pragma once

#include <itchfh/config.hpp>

#include <arpa/inet.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace ttt {

struct program_config {
    char arrangement = 'A';  // note 001, D4
    std::string feed;        // the feed handler's configuration file
    int strategy_cpu = 3, engine_cpu = 4, journal_cpu = 5, sender_cpu = 6;  // journal, sender: the pipeline's
    std::string venue_ip = "127.0.0.1";
    std::uint16_t venue_port = 26500;
    std::string sender_comp_id = "CLIENT", target_comp_id = "VENUE";  // order_venue's
    char journal = 'C';                     // note 001, D6: C (a process crash) or A (power loss)
    bool pipeline = false;                  // the gateway's engine: the direct loop, or its pipeline
    std::string journal_path = "/dev/shm/ttt.journal";
    std::uint64_t journal_mb = 8192;        // C: its fixed capacity; A: pre-filled in steps of this
    std::string instruments = "open-100.txt", limits = "limits.conf";
    // The gateway's tables, for the session's life (its note 007, D3): sized for the day.
    std::uint64_t max_orders = 2'000'000, max_exec_ids = 16'000'000, id_mb = 512;
    // The gateway's store of sent messages (its note 004, amended): reserved for the
    // day, or it reallocates and copies itself in one call past the reserve.
    std::uint64_t store_mb = 2048, store_messages = 16'000'000;
    std::string block = "/ttt";             // control and monitoring
    std::string passed_log;                 // S1: the IDs the risk passed, written at exit (empty: not)
    std::uint64_t end_wait_ms = 5000;       // D8
    // Note 005: the sender's stamp array (empty: no stamping), where the traced orders go, how many.
    std::string stamps, trace;
    std::uint64_t trace_capacity = 8'000'000;
    itchfh::program_config feed_config;     // read from `feed`
};

inline bool parse_program(const std::string& text, const std::filesystem::path& dir, program_config& c, std::string& why) {
    std::map<std::string, std::string> seen;
    std::istringstream in(text);
    std::string line;
    const auto path = [&](const std::string& v) { return std::filesystem::path(v).is_absolute() ? v : (dir / v).string(); };
    for (int n = 1; std::getline(in, line); ++n) {
        if (const auto h = line.find('#'); h != std::string::npos) line.erase(h);
        const auto trim = [](std::string s) {
            const auto a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
        };
        line = trim(line);
        if (line.empty()) continue;
        const auto bad = [&](const std::string& what) {
            why = "line " + std::to_string(n) + ": " + what;
            return false;
        };
        const auto eq = line.find('=');
        if (eq == std::string::npos) return bad("not `key = value`");
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (seen.count(k)) return bad(k + " given twice");
        seen[k] = v;
        std::uint64_t x = 0;
        const auto number = [&](std::uint64_t lo, std::uint64_t hi) {
            if (v.empty() || v.size() > 19 || v.find_first_not_of("0123456789") != std::string::npos) return false;
            x = std::stoull(v);
            return x >= lo && x <= hi;
        };
        if (k == "arrangement") {
            if (v != "A" && v != "B" && v != "C") return bad("arrangement: A, B or C");
            c.arrangement = v[0];
        } else if (k == "feed") {
            c.feed = path(v);
        } else if (k == "strategy_cpu" || k == "engine_cpu" || k == "journal_cpu" || k == "sender_cpu") {
            if (!number(0, 1023)) return bad(k + ": a CPU number");
            (k == "strategy_cpu" ? c.strategy_cpu : k == "engine_cpu" ? c.engine_cpu : k == "journal_cpu" ? c.journal_cpu : c.sender_cpu) =
                static_cast<int>(x);
        } else if (k == "venue") {
            in_addr a{};
            if (::inet_pton(AF_INET, v.c_str(), &a) != 1) return bad("venue: not an IPv4 address");
            c.venue_ip = v;
        } else if (k == "venue_port") {
            if (!number(1, 65535)) return bad("venue_port: a port, 1 to 65535");
            c.venue_port = static_cast<std::uint16_t>(x);
        } else if (k == "sender_comp_id" || k == "target_comp_id") {
            if (v.empty() || v.size() > 32 || v.find_first_of(" =\x01") != std::string::npos) return bad(k + ": 1 to 32 characters, no space or =");
            (k == "sender_comp_id" ? c.sender_comp_id : c.target_comp_id) = v;
        } else if (k == "journal") {
            if (v != "A" && v != "C") return bad("journal: A or C");
            c.journal = v[0];
        } else if (k == "engine") {
            if (v != "direct" && v != "pipeline") return bad("engine: direct or pipeline");
            c.pipeline = v == "pipeline";
        } else if (k == "journal_path") {
            c.journal_path = path(v);
        } else if (k == "journal_mb") {
            if (!number(1, 65536)) return bad("journal_mb: 1 to 65536");
            c.journal_mb = x;
        } else if (k == "instruments" || k == "limits") {
            (k == "instruments" ? c.instruments : c.limits) = path(v);
        } else if (k == "max_orders" || k == "max_exec_ids") {
            if (!number(1024, 1'000'000'000)) return bad(k + ": 1024 to 10^9");
            (k == "max_orders" ? c.max_orders : c.max_exec_ids) = x;
        } else if (k == "id_mb") {
            if (!number(1, 65536)) return bad("id_mb: 1 to 65536");
            c.id_mb = x;
        } else if (k == "store_mb") {
            if (!number(1, 65536)) return bad("store_mb: 1 to 65536");
            c.store_mb = x;
        } else if (k == "store_messages") {
            if (!number(1024, 1'000'000'000)) return bad("store_messages: 1024 to 10^9");
            c.store_messages = x;
        } else if (k == "block") {
            if (v.size() < 2 || v[0] != '/' || v.find('/', 1) != std::string::npos) return bad("block: a shared-memory name (/name)");
            c.block = v;
        } else if (k == "passed_log") {
            c.passed_log = v == "none" ? std::string{} : path(v);
        } else if (k == "stamps") {
            if (v == "none") c.stamps.clear();
            else if (v.size() < 2 || v[0] != '/' || v.find('/', 1) != std::string::npos) return bad("stamps: a shared-memory name (/name) or none");
            else c.stamps = v;
        } else if (k == "trace") {
            c.trace = v == "none" ? std::string{} : path(v);
        } else if (k == "trace_capacity") {
            if (!number(1, 1'000'000'000)) return bad("trace_capacity: 1 to 10^9");
            c.trace_capacity = x;
        } else if (k == "end_wait_ms") {
            if (!number(0, 600'000)) return bad("end_wait_ms: 0 to 600000");
            c.end_wait_ms = x;
        } else {
            return bad("unknown key " + k);
        }
    }
    if (c.feed.empty()) {
        why = "feed: the feed handler's configuration file is required";
        return false;
    }
    if (c.stamps.empty() != c.trace.empty()) {
        why = "stamps and trace: both or neither (note 005)";
        return false;
    }
    if (!itchfh::load_config(c.feed, c.feed_config, why)) return false;
    if (c.arrangement != 'C' && !c.feed_config.record_dir.empty()) {
        why = c.feed + ": record_dir: the program does not record (`itchfh` does, in arrangement C)";
        return false;
    }
    if (c.arrangement == 'C' && c.feed_config.publish.empty()) {
        why = c.feed + ": publish: arrangement C reads `itchfh`'s publisher, so it needs one";
        return false;
    }
    return true;
}

inline bool load_program(const std::string& file, program_config& c, std::string& why) {
    std::ifstream f(file);
    if (!f) {
        why = file + ": cannot read";
        return false;
    }
    std::stringstream s;
    s << f.rdbuf();
    if (!parse_program(s.str(), std::filesystem::path(file).parent_path(), c, why)) {
        why = file + ": " + why;
        return false;
    }
    return true;
}

}  // namespace ttt
