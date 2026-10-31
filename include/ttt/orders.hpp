// Orders to the gateway and its answers back (note 004, D1 and D2).
//   utc_clock      TransactTime from the TSC: UTC at start, ticks since, so
//                  writing an order makes no system call
//   order_writer   an intent as the MsgType and body the gateway's
//                  application ring takes (its note 005, D5): D, F or G, the
//                  fixed parts copied and the variable ones written in place
//   read_answer    a slot from the gateway's ring as an `order_answer`: an
//                  ExecutionReport or OrderCancelReject by its own fields
//                  (ClOrdID, ExecType, LeavesQty, LastQty, LastPx), a request
//                  the gateway refused or did not send as rejected
// Design: docs/design/004-program.md
#pragma once

#include <ttt/events.hpp>
#include <ttt/strategy.hpp>

#include <fixgw/engine.hpp>
#include <fixgw/orders.hpp>

#include <lfipc/bench/clock.hpp>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string_view>
#include <vector>

namespace ttt {

class utc_clock {
public:
    explicit utc_clock(double ticks_per_ns) : per_ns_(ticks_per_ns) {
        timespec r;
        ::clock_gettime(CLOCK_REALTIME, &r);
        t0_ = lfipc::bench::stamp();
        base_ns_ = static_cast<std::int64_t>(r.tv_sec) * 1'000'000'000 + r.tv_nsec;
    }
    std::int64_t now_ns() const noexcept { return base_ns_ + static_cast<std::int64_t>(static_cast<double>(lfipc::bench::stamp() - t0_) / per_ns_); }

private:
    double per_ns_;
    lfipc::bench::ticks t0_;
    std::int64_t base_ns_;
};

namespace detail {

// Digits of v, at p; returns the end.
inline char* write_u64(char* p, std::uint64_t v) noexcept {
    char t[20];
    int n = 0;
    do t[n++] = static_cast<char>('0' + v % 10), v /= 10;
    while (v);
    while (n) *p++ = t[--n];
    return p;
}
inline char* write_digits(char* p, std::uint64_t v, int width) noexcept {
    for (int i = width - 1; i >= 0; --i) p[i] = static_cast<char>('0' + v % 10), v /= 10;
    return p + width;
}
inline char* put(char* p, std::string_view s) noexcept {
    std::memcpy(p, s.data(), s.size());
    return p + s.size();
}
// Days since 1970-01-01 to a civil date (H. Hinnant's algorithm).
inline void civil(std::int64_t days, int& y, unsigned& m, unsigned& d) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (m <= 2);
}

}  // namespace detail

// UTCTimestamp to milliseconds, YYYYMMDD-HH:MM:SS.sss (21 bytes), at p.
inline char* write_utc(char* p, std::int64_t utc_ns) noexcept {
    const std::int64_t ms = utc_ns / 1'000'000, days = ms / 86'400'000, in_day = ms % 86'400'000;
    int y;
    unsigned m, d;
    detail::civil(days, y, m, d);
    p = detail::write_digits(p, static_cast<std::uint64_t>(y), 4);
    p = detail::write_digits(p, m, 2);
    p = detail::write_digits(p, d, 2);
    *p++ = '-';
    p = detail::write_digits(p, static_cast<std::uint64_t>(in_day / 3'600'000), 2);
    *p++ = ':';
    p = detail::write_digits(p, static_cast<std::uint64_t>(in_day / 60'000 % 60), 2);
    *p++ = ':';
    p = detail::write_digits(p, static_cast<std::uint64_t>(in_day / 1000 % 60), 2);
    *p++ = '.';
    return detail::write_digits(p, static_cast<std::uint64_t>(in_day % 1000), 3);
}

// An order message: its MsgType and body, as `fixgw::submit_order` takes them.
struct order_message {
    char type[2];
    std::uint16_t length;
    char body[256];
    std::string_view msg_type() const noexcept { return {type, 1}; }
    std::string_view text() const noexcept { return {body, length}; }
};

class order_writer {
public:
    // Each instrument's Symbol field, written once.
    explicit order_writer(const instruments& ins) : symbols_(ins.size()) {
        for (std::size_t i = 0; i < ins.size(); ++i) {
            const itchfh::symbol& s = ins.symbol(i);
            std::size_t n = s.size();
            while (n && s[n - 1] == ' ') --n;
            symbols_[i] = "55=" + std::string(s.data(), n) + "\x01";
        }
    }

    // D (new order), F (cancel) or G (replace) for the intent: a day limit buy
    // of `quantity` at `price` (four implied decimals), ClOrdID the intent's
    // ID, OrigClOrdID the order's.
    void write(const intent& i, std::int64_t utc_ns, order_message& m) const noexcept {
        char* p = m.body;
        m.type[0] = i.kind == intent::new_order ? 'D' : i.kind == intent::cancel ? 'F' : 'G';
        m.type[1] = 0;
        if (i.kind != intent::new_order) p = detail::write_u64(detail::put(p, "41="), i.orig_id), *p++ = '\x01';
        p = detail::write_u64(detail::put(p, "11="), i.id);
        *p++ = '\x01';
        p = detail::put(p, symbols_[i.slot]);
        p = write_utc(detail::put(p, "54=1\x01" "60="), utc_ns);
        p = detail::write_u64(detail::put(p, "\x01" "38="), i.quantity);
        *p++ = '\x01';
        if (i.kind != intent::cancel) {
            p = detail::write_u64(detail::put(p, "40=2\x01" "44="), i.price / 10'000);
            *p++ = '.';
            p = detail::write_digits(p, i.price % 10'000, 4);
            p = detail::put(p, "\x01" "59=0\x01");
        }
        m.length = static_cast<std::uint16_t>(p - m.body);
    }

private:
    std::vector<std::string> symbols_;
};

// What a slot from the gateway's ring is to the strategy and the risk.
enum class answer_read : std::uint8_t {
    answer,    // `a` holds it
    none,      // nothing for them (a pending report, an order's state already told by its message)
    unmapped,  // something this program does not expect: counted
};

namespace detail {

inline bool parse_id(std::string_view v, std::uint64_t& id) noexcept {
    if (v.empty() || v.size() > 20) return false;
    std::uint64_t x = 0;
    for (const char c : v) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t n = x * 10 + static_cast<std::uint64_t>(c - '0');
        if (n < x) return false;
        x = n;
    }
    id = x;
    return true;
}

}  // namespace detail

inline answer_read read_answer(const fixgw::app_slot& s, order_answer& a) noexcept {
    if (s.kind == fixgw::app_slot::refused || s.kind == fixgw::app_slot::order_state) {
        const fixgw::order_event e = fixgw::slot_event(s);
        if (s.kind == fixgw::app_slot::order_state && e.kind != fixgw::order_event::not_sent) return answer_read::none;
        a = {order_answer::rejected, 0};
        return detail::parse_id(e.id(), a.id) ? answer_read::answer : answer_read::unmapped;
    }
    if (s.kind != fixgw::app_slot::message) return answer_read::unmapped;  // replayed: a fresh session has none (D6)
    // One pass over the message's fields.
    const std::string_view m(s.data, s.length);
    std::string_view type, id, exec_type, leaves, last_qty, last_px;
    std::size_t i = 0;
    while (i < m.size()) {
        std::uint32_t tag = 0;
        while (i < m.size() && m[i] >= '0' && m[i] <= '9') tag = tag * 10 + static_cast<std::uint32_t>(m[i++] - '0');
        if (i >= m.size() || m[i] != '=') return answer_read::unmapped;
        const char* v = m.data() + i + 1;
        const void* end = std::memchr(v, '\x01', m.size() - i - 1);
        const std::size_t n = end ? static_cast<std::size_t>(static_cast<const char*>(end) - v) : m.size() - i - 1;
        const std::string_view f(v, n);
        switch (tag) {
            case 35: type = f; break;
            case 11: id = f; break;
            case 150: exec_type = f; break;
            case 151: leaves = f; break;
            case 32: last_qty = f; break;
            case 31: last_px = f; break;
            default: break;
        }
        i += 1 + n + 1;
    }
    a = {order_answer::rejected, 0};
    if (!detail::parse_id(id, a.id)) return answer_read::unmapped;
    if (type == "9") return answer_read::answer;  // OrderCancelReject: its ClOrdID is the request's
    if (type != "8" || exec_type.size() != 1) return answer_read::unmapped;
    switch (exec_type[0]) {
        case '0': a.kind = order_answer::accepted; return answer_read::answer;
        case '5': a.kind = order_answer::replaced; return answer_read::answer;
        case '4': a.kind = order_answer::cancelled; return answer_read::answer;  // the cancel's ID, or the order's (the venue's own)
        case '8': a.kind = order_answer::rejected; return answer_read::answer;
        case 'F': {
            const auto q = fixgw::detail::parse_fixed(last_qty), p = fixgw::detail::parse_fixed(last_px), l = fixgw::detail::parse_fixed(leaves);
            if (!q || !p || !l) return answer_read::unmapped;
            a.kind = *l == 0 ? order_answer::filled : order_answer::partly_filled;
            a.quantity = static_cast<std::uint32_t>(*q / 100'000'000);
            a.price = static_cast<std::uint32_t>(*p / 10'000);
            return answer_read::answer;
        }
        case 'A':
        case '6':
        case 'E': return answer_read::none;  // pending new, cancel, replace
        default: return answer_read::unmapped;
    }
}

}  // namespace ttt
