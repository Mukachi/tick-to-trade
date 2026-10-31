// The risk checks (note 003, S1 to S4 in small): each check refuses an intent
// built to break it, by its reason, counted, answered `rejected`, nothing
// sent; cancels pass every check; counting at send, a replace in flight at
// both prices; the kill switch by the operator and by the fill throttle; then
// a long random run of book events, intents and answers against the
// reference implementation (the same sends, refusals, answers to the
// strategy, counts, and exposures after every step).
// Spec: docs/design/003-pre-trade-risk.md
#include "check.hpp"

#include <ttt/reference_risk.hpp>
#include <ttt/risk.hpp>

#include <itchfh/publication.hpp>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace ttt;
using ttt::test::check;

struct sent {
    std::vector<ttt::intent> v;
    void send(const ttt::intent& i) { v.push_back(i); }
};

const std::uint8_t live = itchfh::pub::reliability(itchfh::feed_state::live, false, false);

book_event ev(std::uint16_t slot, std::uint32_t bid, std::uint32_t ask, std::uint64_t ts, char trading = 'T') {
    book_event e;
    e.slot = slot;
    e.locate = static_cast<std::uint16_t>(100 + slot);
    e.bid_price = bid;
    e.ask_price = ask;
    e.bid_shares = e.ask_shares = 100;
    e.round_lot = 100;
    e.reliability = live;
    e.trading_state = trading;
    e.timestamp_ns = ts;
    e.sequence = ts;
    return e;
}

std::uint64_t next_id = 1;
ttt::intent mk(intent::kind_t k, std::uint16_t slot, std::uint32_t price, std::uint32_t qty, std::uint64_t orig = 0) {
    return ttt::intent{k, slot, static_cast<std::uint16_t>(100 + slot), order_id(next_id++, slot), orig, price, qty, 0};
}

limits small_limits() {
    limits l;
    l.max_volume = 1'000;
    l.max_value = 1'000'000'000;  // $100,000
    l.collar_bps = 500;
    l.period_collar_bps = 1'000;
    l.period_ns = 60'000'000'000;
    l.max_messages = 1'000;
    l.messages_window_ns = 1'000'000'000;
    l.max_fills = 1'000;
    l.fills_window_ns = 60'000'000'000;
    l.instrument_exposure = 3'000'000'000;  // $300,000
    l.firm_exposure = 5'000'000'000;        // $500,000
    l.permitted = {"A", "B", "C"};
    return l;
}

// A fresh risk on instruments A, B, C, D (D not permitted), the session up, books usable at $100.00 / $100.10.
struct fixture {
    instruments ins{std::vector<std::string_view>{"A", "B", "C", "D"}};
    sent out;
    limits lim;
    std::unique_ptr<risk<sent>> r;
    explicit fixture(const limits& l = small_limits()) : lim(l) {
        for (std::uint16_t loc = 100; loc < 104; ++loc) {
            const char sym[2] = {static_cast<char>('A' + loc - 100), 0};
            ins.directory(loc, to_symbol(sym));
        }
        r = std::make_unique<risk<sent>>(ins, lim, out);
        r->session(true);
        for (std::uint16_t s = 0; s < 4; ++s) r->on(ev(s, 1'000'000, 1'001'000, 1'000));
    }
    // Sends `i`; the refusal's reason if refused (none if it passed).
    refusal send(const ttt::intent& i) {
        const risk_counts before = r->counts();
        const std::size_t n = out.v.size();
        r->intent(i);
        for (int k = 0; k < refusal_kinds; ++k)
            if (r->counts().refused[k] != before.refused[k]) {
                order_answer a;
                const bool answered = r->take_refusal(a) && a.kind == order_answer::rejected && a.id == i.id && out.v.size() == n;
                return answered ? static_cast<refusal>(k) : refusal::none;
            }
        return refusal::none;
    }
    void answer(order_answer::kind_t k, std::uint64_t id, std::uint32_t price = 0, std::uint32_t qty = 0) {
        order_answer to;
        r->answer(order_answer{k, id, price, qty}, to);
    }
};

void each_check() {
    const auto refused = [](const std::string& what, refusal want, auto&& setup) {
        fixture f;
        const ttt::intent i = setup(f);
        const std::size_t n = f.out.v.size();
        const refusal got = f.send(i);
        check(got == want && f.out.v.size() == n && f.r->counts().refused[static_cast<int>(want)] == 1,
              std::string("S1: ") + what + ": refused as " + name(want) + ", counted, answered rejected, nothing sent (got " + name(got) + ")");
    };
    refused("the kill switch engaged", refusal::killed, [](fixture& f) { f.r->kill(); return mk(intent::new_order, 0, 1'000'000, 100); });
    refused("the session down", refusal::session, [](fixture& f) { f.r->session(false); return mk(intent::new_order, 0, 1'000'000, 100); });
    refused("an instrument not permitted (Art. 15(5))", refusal::permission, [](fixture&) { return mk(intent::new_order, 3, 1'000'000, 100); });
    refused("a halted book", refusal::unusable, [](fixture& f) {
        f.r->on(ev(0, 1'000'000, 1'001'000, 2'000, 'H'));
        return mk(intent::new_order, 0, 1'000'000, 100);
    });
    refused("an unsound book", refusal::unusable, [](fixture& f) {
        book_event e = ev(0, 1'000'000, 1'001'000, 2'000);
        e.reliability = itchfh::pub::reliability(itchfh::feed_state::live, true, false);
        f.r->on(e);
        return mk(intent::new_order, 0, 1'000'000, 100);
    });
    refused("a second new order while one is open (15c3-5: duplicative)", refusal::duplicate, [](fixture& f) {
        const ttt::intent a = mk(intent::new_order, 0, 1'000'000, 100);
        f.send(a);
        f.answer(order_answer::accepted, a.id);
        return mk(intent::new_order, 0, 1'000'000, 100);
    });
    refused("a request while one is in flight", refusal::duplicate, [](fixture& f) {
        f.send(mk(intent::new_order, 0, 1'000'000, 100));
        return mk(intent::new_order, 0, 1'000'000, 100);
    });
    refused("1,001 shares (Art. 15(1)(c))", refusal::volume, [](fixture&) { return mk(intent::new_order, 0, 1'000'000, 1'001); });
    refused("$100,000.10 (Art. 15(1)(b))", refusal::value, [](fixture&) { return mk(intent::new_order, 0, 1'000'001, 1'000); });
    refused("above the ask by more than 5% (Art. 15(1)(a))", refusal::collar, [](fixture&) { return mk(intent::new_order, 0, 1'051'051, 100); });
    refused("below the bid by more than 5%", refusal::collar, [](fixture&) { return mk(intent::new_order, 0, 949'999, 100); });
    refused("more than 10% from the period's reference (D2)", refusal::period_collar, [](fixture& f) {
        f.r->on(ev(0, 1'100'000, 1'101'000, 3'000));  // the bid up 10% in the same minute: the reference stays at $100.00
        return mk(intent::new_order, 0, 1'100'001, 100);
    });
    refused("the 1,001st message in a second (Art. 15(1)(d))", refusal::messages, [](fixture& f) {
        for (int k = 0; k < 1'000; ++k) f.r->intent(mk(intent::cancel, 1, 1'000'000, 100, 1));  // cancels count, never refused
        return mk(intent::new_order, 0, 1'000'000, 100);
    });
    refused("past the instrument's exposure (D5)", refusal::instrument_exposure, [](fixture& f) {
        for (int k = 0; k < 3; ++k) {  // three fills of 1,000 at $100: $300,000 held
            const ttt::intent a = mk(intent::new_order, 0, 1'000'000, 1'000);
            f.send(a);
            f.answer(order_answer::filled, a.id, 1'000'000, 1'000);
        }
        return mk(intent::new_order, 0, 1'000'000, 1);
    });
    refused("past the firm's exposure (D5)", refusal::firm_exposure, [](fixture& f) {
        for (std::uint16_t s = 0; s < 2; ++s)
            for (int k = 0; k < 2; ++k) {
                const ttt::intent a = mk(intent::new_order, s, 1'000'000, 1'000);
                f.send(a);
                f.answer(order_answer::filled, a.id, 1'000'000, 1'000);
            }
        const ttt::intent a = mk(intent::new_order, 2, 1'000'000, 1'000);
        f.send(a);
        f.answer(order_answer::accepted, a.id);  // $500,000 now: held and open
        return mk(intent::new_order, 1, 1'000'000, 1);
    });

    // The edges pass
    fixture f;
    check(f.send(mk(intent::new_order, 0, 1'051'050, 100)) == refusal::none && f.send(mk(intent::new_order, 1, 950'000, 100)) == refusal::none &&
              f.send(mk(intent::new_order, 2, 1'000'000, 1'000)) == refusal::none && f.out.v.size() == 3,
          "S1: the edges pass (the limits are inclusive): exactly 5% above the ask, exactly 5% below the bid, exactly 1,000 shares for exactly $100,000");

    // Cancels pass every check
    fixture g;
    g.r->kill();
    g.r->session(false);
    g.r->on(ev(0, 1'000'000, 1'001'000, 2'000, 'H'));
    const std::size_t n = g.out.v.size();
    g.r->intent(mk(intent::cancel, 0, 1'000'000, 100, 99));
    g.r->intent(mk(intent::cancel, 3, 1'000'000, 100, 99));
    check(g.out.v.size() == n + 2 && g.r->counts().cancels == 2, "S1: cancels pass every check: killed, session down, halted, not permitted");
}

void counting() {
    fixture f;
    const ttt::intent a = mk(intent::new_order, 0, 1'000'000, 1'000);
    f.send(a);
    check(f.r->exposure(0) == 1'000'000'000 && f.r->firm_exposure() == 1'000'000'000,
          "S2 (Art. 15(2)): a new order counted from the moment it passes, before any answer ($100,000)");
    f.answer(order_answer::rejected, a.id);
    check(f.r->exposure(0) == 0 && f.r->firm_exposure() == 0, "S2: its rejection releases it");
    const ttt::intent b = mk(intent::new_order, 0, 1'000'000, 1'000);
    f.send(b);
    f.answer(order_answer::accepted, b.id);
    f.answer(order_answer::partly_filled, b.id, 1'000'000, 400);
    check(f.r->exposure(0) == 1'000'000'000 && f.r->position(0) == 400, "S2: a part fill moves 400 shares from open to position: exposure unchanged");
    const ttt::intent c = mk(intent::replace, 0, 990'000, 1'000, b.id);
    f.send(c);
    check(f.r->exposure(0) == 400'000'000 + 600'000'000 + 594'000'000,
          "S2, D3: a replace in flight counted at both prices: position, 600 left at $100.00 and at $99.00");
    f.answer(order_answer::replaced, c.id);
    check(f.r->exposure(0) == 400'000'000 + 594'000'000, "S2: replaced: the new price alone");
    const ttt::intent d = mk(intent::cancel, 0, 990'000, 600, c.id);
    f.send(d);
    check(f.r->exposure(0) == 400'000'000 + 594'000'000, "S2: a cancel in flight lowers nothing until answered");
    f.answer(order_answer::cancelled, d.id);
    check(f.r->exposure(0) == 400'000'000 && f.r->firm_exposure() == 400'000'000, "S2: cancelled: the position alone");
    const ttt::intent e = mk(intent::new_order, 1, 1'000'000, 100);
    f.send(e);
    f.answer(order_answer::filled, e.id, 999'900, 100);
    check(f.r->exposure(1) == 99'990'000 && f.r->position(1) == 100, "S2: a fill at another price is held at the fill's price");
}

void kill_switch() {
    fixture f;
    const ttt::intent a = mk(intent::new_order, 0, 1'000'000, 100), b = mk(intent::new_order, 1, 1'000'000, 100),
                      c = mk(intent::new_order, 2, 1'000'000, 100);
    f.send(a);
    f.send(b);
    f.send(c);
    f.answer(order_answer::accepted, a.id);
    f.answer(order_answer::accepted, b.id);
    const ttt::intent rb = mk(intent::replace, 1, 1'001'000, 100, b.id);
    f.send(rb);  // a replace in flight at the kill
    const std::size_t n = f.out.v.size();
    f.r->kill();
    std::size_t cancels = 0;
    for (std::size_t k = n; k < f.out.v.size(); ++k) cancels += f.out.v[k].kind == intent::cancel && (f.out.v[k].id & kill_bit);
    check(f.r->killed() && cancels == 2 && f.out.v.size() == n + 2, "S3: kill: a cancel of the risk's own for each open order (2), at once");
    check(f.send(mk(intent::new_order, 3, 1'000'000, 100)) == refusal::killed && f.send(mk(intent::replace, 0, 1'001'000, 100, a.id)) == refusal::killed,
          "S3: killed: no new order or replace passes");
    f.answer(order_answer::accepted, c.id);  // acknowledged after the kill
    check(f.out.v.size() == n + 3 && f.out.v.back().kind == intent::cancel && f.out.v.back().orig_id == c.id,
          "S3: an order acknowledged after the kill is cancelled then");
    f.answer(order_answer::replaced, rb.id);  // the replace done before the risk's cancel reached it
    order_answer to;
    const std::uint64_t kill_b = f.out.v[n + 1].id;
    const bool told = f.r->answer(order_answer{order_answer::rejected, kill_b}, to);
    check(!told && f.out.v.back().orig_id == rb.id && (f.out.v.back().id & kill_bit),
          "S3: the risk's cancel rejected (the order replaced meanwhile): the strategy is not told, the new order is cancelled again");
    const bool told_a = f.r->answer(order_answer{order_answer::cancelled, f.out.v[n].id}, to);
    check(told_a && to.kind == order_answer::cancelled && to.id == a.id && f.r->exposure(0) == 0,
          "S3: the risk's cancel done: the strategy told the order is cancelled, by the order's ID; released");
    f.r->reenable();
    check(!f.r->killed() && f.send(mk(intent::new_order, 3, 1'000'000, 100)) == refusal::permission,
          "S3: only reenable() ends it (the next refusal is the instrument's own)");

    limits l = small_limits();
    l.max_fills = 5;
    fixture g(l);
    for (int k = 0; k < 5; ++k) {
        const ttt::intent o = mk(intent::new_order, 0, 1'000'000, 100);
        check(g.send(o) == refusal::none, "fill " + std::to_string(k + 1) + ": a new order passes");
        g.answer(order_answer::accepted, o.id);
        g.answer(order_answer::filled, o.id, 1'000'000, 100);
    }
    check(g.r->killed() && g.r->counts().kills == 1, "S3, D4 (Art. 15(3)): the 5th fill in a minute engages the kill switch");
    g.r->on(ev(0, 1'000'000, 1'001'000, 120'000'000'000));  // two minutes on
    check(g.r->killed(), "S3: it stays engaged once the fills leave the window: only the operator re-enables");
}

// ---- S4 in small: random against the reference

struct logged {
    std::vector<ttt::intent> v;
    void send(const ttt::intent& i) { v.push_back(i); }
};

void random_run() {
    instruments ins{std::vector<std::string_view>{"A", "B", "C", "D"}};
    for (std::uint16_t loc = 100; loc < 104; ++loc) {
        const char sym[2] = {static_cast<char>('A' + loc - 100), 0};
        ins.directory(loc, to_symbol(sym));
    }
    limits l = small_limits();
    l.max_messages = 12;
    l.messages_window_ns = 400'000;
    l.max_fills = 30;
    l.fills_window_ns = 20'000'000;
    l.period_collar_bps = 200;
    l.period_ns = 50'000'000;
    l.instrument_exposure = 600'000'000;
    l.firm_exposure = 1'500'000'000;
    logged fo, ro;
    risk<logged> fast(ins, l, fo);
    reference_risk<logged> ref(ins, l, ro);
    fast.session(true);
    ref.session(true);
    std::mt19937_64 rng(5);
    std::deque<ttt::intent> venue;  // sent, not yet answered
    std::vector<std::uint64_t> resting(4, 0);
    std::vector<std::uint32_t> resting_price(4, 0);
    std::uint64_t ts = 1'000, steps = 0;
    std::uint64_t counter = 1;
    bool agree = true;
    std::string where;
    std::size_t compared = 0;
    const auto both_answer = [&](const order_answer& a) {
        order_answer tf, tr;
        const bool f1 = fast.answer(a, tf), r1 = ref.answer(a, tr);
        if (f1 != r1 || (f1 && !(tf == tr))) agree = false, where = "an answer to the strategy";
    };
    for (; steps < 300'000 && agree; ++steps) {
        const std::uint16_t s = static_cast<std::uint16_t>(rng() % 4);
        const int what = static_cast<int>(rng() % 10);
        if (what < 3) {
            ts += rng() % 20'000;
            const std::uint32_t bid = static_cast<std::uint32_t>(990'000 + 1'000 * (rng() % 30));
            const char st = rng() % 30 == 0 ? 'H' : 'T';
            const book_event e = ev(s, rng() % 25 == 0 ? 0 : bid, bid + 1'000, ts, st);
            fast.on(e);
            ref.on(e);
        } else if (what < 6) {
            const intent::kind_t k = rng() % 3 == 0 ? intent::new_order : rng() % 2 ? intent::replace : intent::cancel;
            const std::uint64_t orig = resting[s] && rng() % 8 ? resting[s] : order_id(rng() % 1'000, s);
            if (k == intent::new_order && resting[s] && rng() % 4) continue;  // mostly one order an instrument, as the strategy
            const std::uint32_t price = static_cast<std::uint32_t>(940'000 + 1'000 * (rng() % 120));
            const std::uint32_t qty = static_cast<std::uint32_t>(rng() % 20 == 0 ? 1'100 : 100 * (1 + rng() % 10));
            const ttt::intent i{k, s, static_cast<std::uint16_t>(100 + s), order_id(counter++, s), k == intent::new_order ? 0 : orig, price, qty, 0};
            fast.intent(i);
            ref.intent(i);
            if (rng() % 5'000 == 0) fast.kill(), ref.kill();
            if (rng() % 200 == 0) fast.reenable(), ref.reenable();
        } else if (!venue.empty()) {
            const ttt::intent i = venue.front();
            venue.pop_front();
            const std::uint16_t vs = i.slot;
            const int how = static_cast<int>(rng() % 10);
            if (how == 0) both_answer({order_answer::rejected, i.id});
            else if (i.kind == intent::new_order) {
                both_answer({order_answer::accepted, i.id});
                resting[vs] = i.id;
                resting_price[vs] = i.price;
                if (how < 6) both_answer({order_answer::partly_filled, i.id, i.price, i.quantity / 2});
                if (how < 4) both_answer({order_answer::filled, i.id, i.price, i.quantity - i.quantity / 2}), resting[vs] = 0;
            } else if (i.kind == intent::replace) {
                if (resting[vs] == i.orig_id) {
                    both_answer({order_answer::replaced, i.id});
                    resting[vs] = i.id;
                    resting_price[vs] = i.price;
                } else both_answer({order_answer::rejected, i.id});
            } else {
                if (resting[vs] == i.orig_id) both_answer({order_answer::cancelled, i.id}), resting[vs] = 0;
                else both_answer({order_answer::rejected, i.id});
            }
        }
        // What each sent, refused and holds
        if (fo.v.size() != ro.v.size() || !std::equal(fo.v.begin() + static_cast<std::ptrdiff_t>(compared), fo.v.end(), ro.v.begin() + static_cast<std::ptrdiff_t>(compared)))
            agree = false, where = "the intents sent";
        for (std::size_t k = compared; k < fo.v.size() && agree; ++k) venue.push_back(fo.v[k]);
        compared = fo.v.size();
        order_answer af, ar;
        for (;;) {
            const bool gf = fast.take_refusal(af), gr = ref.take_refusal(ar);
            if (gf != gr || (gf && !(af == ar))) agree = false, where = "the refusals";
            if (!gf || !gr) break;
        }
        if (!(fast.counts() == ref.counts()) || fast.killed() != ref.killed() || fast.firm_exposure() != ref.firm_exposure())
            agree = false, where = "the counts, kill switch or firm exposure";
        for (std::size_t k = 0; k < 4 && agree; ++k)
            if (fast.exposure(k) != ref.exposure(k) || fast.position(k) != ref.position(k)) agree = false, where = "an instrument's exposure";
    }
    const risk_counts& c = fast.counts();
    std::string reasons;
    bool every = true;
    for (int k = 1; k < refusal_kinds; ++k) {
        if (k == static_cast<int>(refusal::session) || k == static_cast<int>(refusal::permission) || k == static_cast<int>(refusal::value)) continue;
        every &= c.refused[k] > 0;
        reasons += std::string(reasons.empty() ? "" : ", ") + name(static_cast<refusal>(k)) + " " + std::to_string(c.refused[k]);
    }
    check(agree, "S4 in small: " + std::to_string(steps) + " random steps: the checks and the reference agree on every send, refusal, answer, count and exposure" +
                     (agree ? "" : " (differ at step " + std::to_string(steps) + ": " + where + ")"));
    check(every && c.kills > 0 && c.fills > 0, "the run refused by every reason it can reach (" + reasons + "), killed " + std::to_string(c.kills) +
                                                   " times, " + std::to_string(c.fills) + " fills");
}

void configuration() {
    limits d, l;
    std::string why;
    const bool ok = read_limits(TTT_CONFIG_DIR "/limits.conf", l, why);
    check(ok && l.max_volume == d.max_volume && l.max_value == d.max_value && l.collar_bps == d.collar_bps &&
              l.period_collar_bps == d.period_collar_bps && l.period_ns == d.period_ns && l.max_messages == d.max_messages &&
              l.messages_window_ns == d.messages_window_ns && l.max_fills == d.max_fills && l.fills_window_ns == d.fills_window_ns &&
              l.instrument_exposure == d.instrument_exposure && l.firm_exposure == d.firm_exposure && l.permitted.empty(),
          "config/limits.conf reads back as the defaults " + why);
    const std::string path = "/tmp/ttt-limits-" + std::to_string(::getpid()) + ".conf";
    const auto with = [&](const std::string& text) {
        std::ofstream(path) << text;
        limits x;
        std::string w;
        const bool r = read_limits(path, x, w);
        return std::make_pair(r ? x : limits{}, r ? std::string{} : w);
    };
    const auto [p, pw] = with("permitted = TQQQ, QQQ ,SPY\nmax_volume = 500 # a comment\n");
    check(pw.empty() && p.permitted == std::vector<std::string>{"TQQQ", "QQQ", "SPY"} && p.max_volume == 500, "a permission list and a comment read");
    check(!with("max_volme = 5\n").second.empty() && !with("max_volume = 5x\n").second.empty() && !with("max_volume = 0\n").second.empty() &&
              !with("max_volume = 99999999999\n").second.empty() && !with("max_volume\n").second.empty(),
          "refused: an unknown key, a bad number, zero, out of range, no value (" + with("max_volme = 5\n").second + ")");
    std::remove(path.c_str());
}

}  // namespace

int main() {
    configuration();
    each_check();
    counting();
    kill_switch();
    random_run();
    return ttt::test::result();
}
