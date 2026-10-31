// What the strategy is told about the market (note 002): a book event is an
// instrument's whole state after a change (best bid and ask, its trading
// state, its reliability, its round lot), never a difference, so a later
// event replaces an earlier one entirely (D4). The instruments traded are
// named by symbol and given a slot each, in the order configured; the Stock
// Directory gives each one's Stock Locate, and every other locate is ignored
// with one table lookup.
// Design: docs/design/002-book-events-strategy.md
#pragma once

#include <itchfh/messages.hpp>
#include <itchfh/publication.hpp>  // pub::reliability, pub::current

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ttt {

inline constexpr std::uint16_t no_slot = 0xFFFF;
inline constexpr std::size_t max_instruments = 1024;

// One cache line: what the sink pushes through the ring to the strategy.
struct alignas(64) book_event {
    std::uint64_t sequence = 0;      // of the ITCH message that caused it (0: a change of the feed's state)
    std::uint64_t timestamp_ns = 0;  // that message's, since midnight
    std::uint64_t bid_shares = 0, ask_shares = 0;
    std::uint32_t bid_price = 0, ask_price = 0;  // four implied decimals; 0: the side is empty
    std::uint32_t round_lot = 0;                 // the directory's Round Lot Size
    std::uint16_t locate = 0;
    std::uint16_t slot = 0;      // the instrument's place in the configured list
    std::uint8_t reliability = 0;  // the feed handler's (its note 008, D4)
    char trading_state = 0;        // H P Q T; 0 before the day's first Stock Trading Action

    // The state alone, without what caused it: equal states send no event.
    bool same_state(const book_event& o) const {
        return bid_price == o.bid_price && ask_price == o.ask_price && bid_shares == o.bid_shares && ask_shares == o.ask_shares &&
               reliability == o.reliability && trading_state == o.trading_state && round_lot == o.round_lot;
    }
    bool operator==(const book_event&) const = default;
};
static_assert(sizeof(book_event) == 64 && std::is_trivially_copyable_v<book_event>);

// Usable (note 002): the feed live, the instrument's book sound and its status
// restored, and trading. The strategy also needs a best bid to follow.
inline bool usable(const book_event& e) {
    return itchfh::pub::current(e.reliability) && e.trading_state == 'T' && e.bid_price != 0;
}

inline itchfh::symbol to_symbol(std::string_view s) {
    itchfh::symbol k;
    k.fill(' ');
    std::memcpy(k.data(), s.data(), s.size() < k.size() ? s.size() : k.size());
    return k;
}

// The configured instruments, and the locate of each once its Stock
// Directory has come.
class instruments {
public:
    explicit instruments(const std::vector<std::string_view>& symbols) : by_locate_(std::make_unique<std::uint16_t[]>(65'536)) {
        for (std::size_t i = 0; i < 65'536; ++i) by_locate_[i] = no_slot;
        for (const std::string_view s : symbols)
            if (symbols_.size() < max_instruments) symbols_.push_back(to_symbol(s));
        locates_.assign(symbols_.size(), 0);
        known_.assign(symbols_.size(), false);
    }
    // A Stock Directory: the slot it gives its locate, or no_slot if not traded.
    std::uint16_t directory(std::uint16_t locate, const itchfh::symbol& s) {
        for (std::size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == s) {
                if (known_[i] && locates_[i] != locate) by_locate_[locates_[i]] = no_slot;
                locates_[i] = locate;
                known_[i] = true;
                by_locate_[locate] = static_cast<std::uint16_t>(i);
                return static_cast<std::uint16_t>(i);
            }
        return no_slot;
    }
    std::uint16_t slot(std::uint16_t locate) const { return by_locate_[locate]; }
    std::size_t size() const { return symbols_.size(); }
    bool known(std::size_t slot) const { return known_[slot]; }
    std::uint16_t locate(std::size_t slot) const { return locates_[slot]; }
    const itchfh::symbol& symbol(std::size_t slot) const { return symbols_[slot]; }

private:
    std::unique_ptr<std::uint16_t[]> by_locate_;
    std::vector<itchfh::symbol> symbols_;
    std::vector<std::uint16_t> locates_;
    std::vector<bool> known_;
};

}  // namespace ttt
