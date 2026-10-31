// Subjects for check_hot_path.py (note 002, S5): the feed thread's work for
// a message (the feed handler's handler on the default book, the strategy's
// sink, the push into D4's ring), for a change of the feed's state and for
// its tick (the flush of what is aside); the adapter's for a publisher's
// update; the strategy's for a book event and for an answer, with an output
// that does nothing; and the strategy thread's pop. And the risk checks (note
// 003, S6): an intent, an answer, a book event, a refusal taken, the kill.
// And the program's strategy thread (note 004, S4): a book event through the
// risk and the strategy to an order message in the gateway's ring, and a pass
// (the gateway's answers read, its session's state, a command, the counters).
// Spec: docs/design/002-book-events-strategy.md, docs/design/003-pre-trade-risk.md, docs/design/004-program.md
#include <ttt/adapter.hpp>
#include <ttt/event_ring.hpp>
#include <ttt/risk.hpp>
#include <ttt/sink.hpp>
#include <ttt/strategy.hpp>
#include <ttt/trader.hpp>

using ring = ttt::event_ring<>;
using fed = ttt::strategy_feed<ring>;

struct quiet_out {
    void intent(const ttt::intent&) {}
};
struct quiet_send {
    void send(const ttt::intent&) {}
};

extern "C" [[gnu::noinline]] void ttt_feed_message(fed& f, std::uint64_t sequence, const itchfh::message& m) { f.on(sequence, m); }
extern "C" [[gnu::noinline]] void ttt_feed_state(fed& f, itchfh::feed_state s, std::uint64_t next) { f.feed(s, next); }
extern "C" [[gnu::noinline]] void ttt_feed_tick(fed& f, std::int64_t now) { f.tick(now); }
extern "C" [[gnu::noinline]] void ttt_adapter_update(ttt::adapter<ring>& a, const itchfh::pub::update& u) { a.on(u); }
extern "C" [[gnu::noinline]] void ttt_strategy_event(ttt::strategy<quiet_out>& s, const ttt::book_event& e) { s.on(e); }
extern "C" [[gnu::noinline]] void ttt_strategy_answer(ttt::strategy<quiet_out>& s, const ttt::order_answer& a) { s.answer(a); }
extern "C" [[gnu::noinline]] bool ttt_ring_pop(ring& r, ttt::book_event& e) { return r.pop(e); }
// Note 003, S6: the risk checks
extern "C" [[gnu::noinline]] void ttt_risk_intent(ttt::risk<quiet_send>& r, const ttt::intent& i) { r.intent(i); }
extern "C" [[gnu::noinline]] bool ttt_risk_answer(ttt::risk<quiet_send>& r, const ttt::order_answer& a, ttt::order_answer& to) { return r.answer(a, to); }
extern "C" [[gnu::noinline]] void ttt_risk_event(ttt::risk<quiet_send>& r, const ttt::book_event& e) { r.on(e); }
extern "C" [[gnu::noinline]] bool ttt_risk_refusal(ttt::risk<quiet_send>& r, ttt::order_answer& a) { return r.take_refusal(a); }
extern "C" [[gnu::noinline]] void ttt_risk_kill(ttt::risk<quiet_send>& r) { r.kill(); }
// Note 004, S4: the strategy thread
struct fake_engine {
    bool logged_on() const { return true; }
};
using program_trader = ttt::trader<fake_engine>;
extern "C" [[gnu::noinline]] void ttt_trader_event(program_trader& t, const ttt::book_event& e) { t.push(e); }
extern "C" [[gnu::noinline]] bool ttt_trader_pass(program_trader& t, ttt::mon::block* b) { return t.pass(b); }

// Never run: the subjects are only read from the binary.
int main(int argc, char**) {
    if (argc > 100) {
        auto r = std::make_unique<ring>();
        ttt::instruments ins({"A"});
        auto b = std::make_unique<itchfh::default_book>();
        fed f(*b, ins, *r);
        itchfh::message m{};
        ttt_feed_message(f, 1, m);
        ttt_feed_state(f, itchfh::feed_state::live, 1);
        ttt_feed_tick(f, 0);
        ttt::adapter<ring> a(ins, *r);
        ttt_adapter_update(a, itchfh::pub::update{});
        quiet_out q;
        ttt::strategy<quiet_out> s(1, q);
        ttt::book_event e;
        ttt_strategy_event(s, e);
        ttt_strategy_answer(s, ttt::order_answer{});
        quiet_send qs;
        ttt::risk<quiet_send> k(ins, ttt::limits{}, qs);
        ttt::order_answer oa{}, to{};
        ttt_risk_intent(k, ttt::intent{});
        ttt_risk_answer(k, oa, to);
        ttt_risk_event(k, e);
        ttt_risk_refusal(k, oa);
        ttt_risk_kill(k);
        fake_engine g;
        auto to_gw = std::make_unique<fixgw::app_ring>(), from_gw = std::make_unique<fixgw::app_ring>();
        program_trader t(ins, ttt::limits{}, g, *to_gw, *from_gw, 2.2, 16, 'A');
        ttt_trader_event(t, e);
        ttt_trader_pass(t, nullptr);
        return ttt_ring_pop(*r, e);
    }
    return 0;
}
