// The strategy (note 002, S2 and S3): note 002's table state by state, with
// built events and answers, then a long random run of events and answers
// against the reference strategy (the same intents, and the fast strategy's
// state checked after every step).
// Spec: docs/design/002-book-events-strategy.md
#include "check.hpp"
#include "venue_model.hpp"

#include <ttt/reference_strategy.hpp>
#include <ttt/strategy.hpp>

#include <itchfh/publication.hpp>

#include <algorithm>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace ttt;
using ttt::test::check;
using ttt::test::venue;
using ttt::test::venue_out;

struct intents {
    std::vector<ttt::intent> v;
    void intent(const ttt::intent& i) { v.push_back(i); }
};

const std::uint8_t live = itchfh::pub::reliability(itchfh::feed_state::live, false, false);

book_event ev(std::uint32_t bid, std::uint64_t seq, char trading = 'T', std::uint8_t rel = live, std::uint16_t slot = 0) {
    book_event e;
    e.sequence = seq;
    e.bid_price = bid;
    e.bid_shares = bid ? 100 : 0;
    e.ask_price = bid ? bid + 100 : 0;
    e.ask_shares = bid ? 100 : 0;
    e.round_lot = 100;
    e.locate = static_cast<std::uint16_t>(10 + slot);
    e.slot = slot;
    e.reliability = rel;
    e.trading_state = trading;
    return e;
}

bool is(const intent& i, intent::kind_t k, std::uint32_t price, std::uint64_t orig = 0) {
    return i.kind == k && i.price == price && i.orig_id == orig && i.quantity == 100 && i.locate == 10;
}

void table() {
    intents out;
    strategy<intents> s(1, out, {7});
    std::size_t n = 0;
    const auto sent = [&] { return out.v.size() - n; };

    // No order
    s.on(ev(1000, 1));
    check(sent() == 1 && is(out.v[n], intent::new_order, 1000) && out.v[n].id == order_id(7, 0) && out.v[n].sequence == 1,
          "no order, a usable event with a bid: new at the bid, ID from the configured counter, the cause's sequence");
    const std::uint64_t first = out.v[n].id;
    n = out.v.size();

    // In flight: remember
    s.on(ev(1100, 2));
    s.on(ev(1200, 3));
    check(sent() == 0, "a new in flight: newer bids remembered, nothing sent");
    s.answer({order_answer::accepted, first});
    check(sent() == 1 && is(out.v[n], intent::replace, 1200, first) && out.v[n].sequence == 0,
          "accepted: then the newest remembered bid, a replace of the order (caused by an answer: sequence 0)");
    const std::uint64_t rep = out.v[n].id;
    n = out.v.size();
    s.answer({order_answer::replaced, rep});
    check(sent() == 0 && s.order_of(0) == rep && s.price_of(0) == 1200 && !s.pending(0).kind, "replaced: the order has the request's ID and price");

    // D1: the trigger
    book_event e = ev(1200, 4);
    e.bid_shares = 500;
    s.on(e);
    e = ev(1200, 5);
    e.ask_price = 1250;
    s.on(e);
    check(sent() == 0, "D1: a change of the bid's shares, or of the ask, sends nothing");
    s.on(ev(1300, 6));
    check(sent() == 1 && is(out.v[n], intent::replace, 1300, rep), "resting: a new bid, a replace to it (D2: one request)");
    const std::uint64_t rep2 = out.v[n].id;
    n = out.v.size();
    s.on(ev(1200, 7));  // back to the order's price while the replace is in flight
    s.answer({order_answer::replaced, rep2});
    check(sent() == 1 && is(out.v[n], intent::replace, 1200, rep2), "a bid remembered during a replace is acted on once it is answered");
    s.answer({order_answer::replaced, out.v[n].id});
    const std::uint64_t cur = out.v[n].id;
    n = out.v.size();

    // S3: unusable
    s.on(ev(1200, 8, 'H'));
    check(sent() == 1 && out.v[n].kind == intent::cancel && out.v[n].orig_id == cur, "S3: halted, the resting order cancelled");
    const std::uint64_t can = out.v[n].id;
    n = out.v.size();
    s.on(ev(1300, 9, 'T'));
    check(sent() == 0, "a cancel in flight: a usable new bid remembered");
    s.answer({order_answer::cancelled, can});
    check(sent() == 1 && is(out.v[n], intent::new_order, 1300), "cancelled: then the remembered bid, a new order");
    s.answer({order_answer::accepted, out.v[n].id});
    const std::uint64_t o2 = out.v[n].id;
    n = out.v.size();

    for (const auto& [what, rel, trading, bid] :
         std::vector<std::tuple<std::string, std::uint8_t, char, std::uint32_t>>{
             {"the book unsound", itchfh::pub::reliability(itchfh::feed_state::live, true, false), 'T', 1300},
             {"the status not restored", itchfh::pub::reliability(itchfh::feed_state::live, false, true), 'T', 1300},
             {"the feed gapped", itchfh::pub::reliability(itchfh::feed_state::gapped, false, false), 'T', 1300},
             {"the feed lost", itchfh::pub::reliability(itchfh::feed_state::lost, false, false), 'T', 1300},
             {"paused", live, 'P', 1300},
             {"quotation only", live, 'Q', 1300},
             {"no bid", live, 'T', 0}}) {
        intents o;
        strategy<intents> t(1, o);
        t.on(ev(1000, 1));
        t.answer({order_answer::accepted, o.v[0].id});
        t.on(ev(bid, 2, trading, rel));
        check(o.v.size() == 2 && o.v[1].kind == intent::cancel, "S3: " + what + ": the order cancelled");
        intents o3;
        strategy<intents> u(1, o3);
        u.on(ev(bid, 1, trading, rel));
        check(o3.v.empty(), "S3: " + what + ": no new order");
    }

    // A fill, a rejection
    s.answer({order_answer::filled, o2});
    check(sent() == 0 && s.order_of(0) == 0, "filled: no order, nothing sent");
    s.on(ev(1300, 10));
    check(sent() == 0, "no order and the bid unchanged: nothing (waits for a new bid)");
    s.on(ev(1400, 11));
    check(sent() == 1 && out.v[n].kind == intent::new_order, "a new bid: new");
    s.answer({order_answer::rejected, out.v[n].id});
    n = out.v.size();
    check(s.order_of(0) == 0 && !s.pending(0).kind, "rejected new: no order");
    s.on(ev(1400, 12));
    check(sent() == 0, "after a rejection, the same bid sends nothing (no retry loop)");
    s.on(ev(1500, 13));
    s.answer({order_answer::accepted, out.v[n].id});
    const std::uint64_t o3 = out.v[n].id;
    n = out.v.size();
    s.on(ev(1600, 14));
    const std::uint64_t r3 = out.v[n].id;
    n = out.v.size();
    s.answer({order_answer::partly_filled, o3});
    check(s.order_of(0) == o3 && s.pending(0).id == r3, "partly filled: the order rests on");
    s.answer({order_answer::filled, o3});
    check(sent() == 0 && s.order_of(0) == 0 && s.pending(0).id == r3, "filled under a replace in flight: the order gone, the replace still awaited");
    s.on(ev(1700, 15));
    s.answer({order_answer::rejected, r3});
    check(sent() == 1 && is(out.v[n], intent::new_order, 1700), "the replace rejected: no order, then the remembered bid as a new order");
    n = out.v.size();

    // Unusable then usable at the same bid
    intents o4;
    strategy<intents> t(1, o4);
    t.on(ev(1000, 1));
    t.answer({order_answer::accepted, o4.v[0].id});
    t.on(ev(1000, 2, 'H'));
    t.answer({order_answer::cancelled, o4.v[1].id});
    t.on(ev(1000, 3, 'T'));
    check(o4.v.size() == 3 && o4.v[2].kind == intent::new_order && o4.v[2].price == 1000,
          "D1: the first usable event after an unusable one is a new bid, even at the same price");

    // Answers for no order or request
    const auto before = s.counts().unknown_answers;
    s.answer({order_answer::accepted, order_id(999, 0)});
    s.answer({order_answer::filled, order_id(999, 0)});
    s.answer({order_answer::rejected, order_id(1, 5)});
    check(s.counts().unknown_answers == before + 3 && sent() == 0, "an answer with no order or request of its ID: counted, nothing changes");
}

void random_run() {
    constexpr std::size_t slots = 5;
    // Each strategy with its own venue, seeded alike: equal intents give equal answers.
    struct fast_side {
        venue_out<strategy<fast_side>>* out;
        void intent(const ttt::intent& i) { out->intent(i); }
    };
    venue<strategy<fast_side>> fv(slots, 7);
    venue_out<strategy<fast_side>> fo{&fv, {}};
    fast_side fs{&fo};
    strategy<fast_side> fast(slots, fs, {100});

    struct ref_side {
        venue_out<reference_strategy<ref_side>>* out;
        void intent(const ttt::intent& i) { out->intent(i); }
    };
    venue<reference_strategy<ref_side>> rv(slots, 7);
    venue_out<reference_strategy<ref_side>> ro{&rv, {}};
    ref_side rs{&ro};
    reference_strategy<ref_side> ref(rs, {100});

    std::mt19937_64 rng(11);
    const char states[] = {'T', 'T', 'T', 'T', 'T', 'T', 'H', 'P', 'Q'};
    bool agree = true;
    std::size_t at = 0, compared = 0;
    for (std::uint64_t k = 1; k <= 1'000'000 && agree; ++k) {
        const std::uint16_t slot = static_cast<std::uint16_t>(rng() % slots);
        const std::uint32_t bid = static_cast<std::uint32_t>(rng() % 20 == 0 ? 0 : 1000 + 100 * (rng() % 6));
        std::uint8_t rel = live;
        if (rng() % 40 == 0) rel = itchfh::pub::reliability(rng() % 2 ? itchfh::feed_state::gapped : itchfh::feed_state::live, rng() % 2, false);
        book_event e = ev(bid, k, states[rng() % sizeof(states)], rel, slot);
        e.bid_shares = 100 + rng() % 3;
        e.locate = static_cast<std::uint16_t>(10 + slot);
        fast.on(e);
        ref.on(e);
        fv.step(fast);
        rv.step(ref);
        // The intents new since the last step
        if (fo.log.size() != ro.log.size() || !std::equal(fo.log.begin() + compared, fo.log.end(), ro.log.begin() + compared)) {
            agree = false;
            at = k;
        }
        compared = fo.log.size();
        // At most one request in flight, and one order, per instrument
        for (std::size_t s = 0; s < slots && agree; ++s) {
            std::size_t in_flight = 0;
            for (const ttt::intent& i : fv.waiting) in_flight += i.slot == s;
            if (in_flight > 1) agree = false, at = k;
        }
    }
    check(agree && !fo.log.empty(), "S2: a million random events and answers: the strategy's intents equal the reference strategy's (" +
                                        std::to_string(fo.log.size()) + " intents; " + (agree ? "agree" : "differ at event " + std::to_string(at)) +
                                        "), and never two requests in flight for an instrument");
    const auto& c = fast.counts();
    check(c.news > 0 && c.replaces > 0 && c.cancels > 0, "every kind of intent was sent (" + std::to_string(c.news) + " new, " +
                                                            std::to_string(c.replaces) + " replace, " + std::to_string(c.cancels) + " cancel)");
}

}  // namespace

int main() {
    table();
    random_run();
    return ttt::test::result();
}
