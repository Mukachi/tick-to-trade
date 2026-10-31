// Tests for include/ttt/orders.hpp (note 004, S5; D1, D2). Intents written as
// order messages go through the gateway's direct engine, over loopback, to a
// venue session running the gateway's venue simulator (as `order_venue`), and
// what comes back is read as answers: every row of D2's table. The rows the
// simulator does not produce behind the gateway (ExecType Rejected, an
// OrderCancelReject, the venue's own cancel, an order not sent) are read from
// slots built as the gateway delivers them. And TransactTime's form.
// Design: docs/design/004-program.md
#include "check.hpp"

#include <ttt/orders.hpp>

#include <lfipc/bench/calibrate.hpp>

#include <fixgw/venue_sim.hpp>

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace fixgw;
using ttt::test::check;

struct no_journal {
    bool append(record_kind, session_time, std::string_view, std::string_view) { return true; }
    durability make_durable(session_time) { return durability::durable; }
    durability poll(session_time) { return durability::durable; }
};
struct yes {
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};
struct venue_store {
    std::map<std::uint64_t, std::string> m;
    void record(std::uint64_t s, std::string_view b) { m[s] = std::string(b); }
    std::string_view lookup(std::uint64_t s) {
        const auto it = m.find(s);
        return it == m.end() ? std::string_view{} : std::string_view(it->second);
    }
    void reset() { m.clear(); }
};
// The venue's side, as order_venue's.
struct venue_sink {
    tcp_connection* c = nullptr;
    venue_sim* sim = nullptr;
    session<venue_sink, venue_store>* self = nullptr;
    void write(std::string_view b) { c->write(b); }
    void disconnect() { c->close(); }
    void deliver(const message_view<>& m) {
        sim->on_message(1, m, [&](int s, std::string_view t, const std::string& b) {
            if (s == 1) self->send(t, b, clock_now());
        });
    }
    void log(session_log, std::string_view, std::uint64_t) {}
    bool resend(const message_view<>&) { return true; }
    bool release(std::string_view, std::string_view) { return true; }
};

std::string text(const ttt::order_answer& a) {
    static const char* k[] = {"?", "accepted", "replaced", "cancelled", "rejected", "filled", "partly filled"};
    std::string s = std::string(k[a.kind]) + " " + std::to_string(a.id);
    if (a.kind == ttt::order_answer::filled || a.kind == ttt::order_answer::partly_filled)
        s += " " + std::to_string(a.quantity) + " at " + std::to_string(a.price);
    return s;
}

void transact_time() {
    char b[32];
    // 2020-01-30 09:30:00.123 UTC
    const std::int64_t ns = (1'580'376'600LL * 1000 + 123) * 1'000'000 + 456'789;
    const std::string s(b, static_cast<std::size_t>(ttt::write_utc(b, ns) - b));
    const std::string leap(b, static_cast<std::size_t>(ttt::write_utc(b, 951'782'400LL * 1'000'000'000) - b));  // 2000-02-29
    check(s == "20200130-09:30:00.123" && leap == "20000229-00:00:00.000", "TransactTime: " + s + ", " + leap + " (UTCTimestamp to ms)");
    const auto cal = lfipc::bench::calibrate(2, 50);
    const ttt::utc_clock u(cal.ticks_per_ns);
    timespec r;
    ::clock_gettime(CLOCK_REALTIME, &r);
    const std::int64_t off = u.now_ns() - (static_cast<std::int64_t>(r.tv_sec) * 1'000'000'000 + r.tv_nsec);
    check(off > -1'000'000 && off < 1'000'000, "utc_clock from the TSC within 1 ms of CLOCK_REALTIME (" + std::to_string(off) + " ns)");
}

void through_the_gateway() {
    const std::vector<std::string_view> names{"AAA", "BBB"};
    const ttt::instruments ins(names);
    const ttt::order_writer w(ins);
    const auto cal = lfipc::bench::calibrate(2, 50);
    const ttt::utc_clock utc(cal.ticks_per_ns);

    tcp_listener l;
    l.open(0);
    engine_config ec;
    ec.port = l.port();
    session_config cc;
    cc.role = session_role::initiator;
    cc.sender_comp_id = "TTT";
    cc.target_comp_id = "VENUE";
    order_config oc;
    oc.prefault = false;
    no_journal j;
    yes y;
    auto in = std::make_unique<app_ring>(), out = std::make_unique<app_ring>();
    engine<no_journal, yes> e(cc, ec, j, y, *in, *out, oc);
    std::atomic<bool> stop{false};
    std::thread t([&] { e.run(stop); });

    session_config vcfg;
    vcfg.role = session_role::acceptor;
    vcfg.sender_comp_id = "VENUE";
    vcfg.target_comp_id = "TTT";
    vcfg.app_msg_types = "D F G";
    tcp_connection vc;
    venue_sim sim;
    venue_store vst;
    venue_sink vs;
    vs.c = &vc;
    vs.sim = &sim;
    session<venue_sink, venue_store> v(vcfg, vs, vst);
    vs.self = &v;
    int fd = -1;
    for (int k = 0; k < 2'000'000 && fd < 0; ++k) fd = l.accept();
    vc.attach(fd);
    v.on_connected(clock_now());
    std::string inbox;
    std::vector<ttt::order_answer> got;
    int unmapped = 0;
    const auto pump = [&] {
        char b[4096];
        long r;
        while ((r = vc.read_some(b, sizeof(b))) > 0) inbox.append(b, static_cast<std::size_t>(r));
        if (!inbox.empty()) inbox.erase(0, v.on_bytes(inbox.data(), inbox.size(), clock_now()));
        vc.flush();
        v.on_tick(clock_now());
        app_slot s;
        while (out->try_pop(s)) {
            ttt::order_answer a;
            const ttt::answer_read r2 = ttt::read_answer(s, a);
            if (r2 == ttt::answer_read::answer) got.push_back(a);
            else if (r2 == ttt::answer_read::unmapped) ++unmapped;
        }
    };
    const auto until = [&](std::size_t n) {
        const std::int64_t give_up = clock_now().mono_ns + 5'000'000'000;
        while (got.size() < n && clock_now().mono_ns < give_up) pump();
        return got.size() >= n;
    };
    const std::int64_t give_up = clock_now().mono_ns + 5'000'000'000;
    while (!e.logged_on() && clock_now().mono_ns < give_up) pump();
    check(e.logged_on(), "the engine logged on to the venue session");

    const auto send = [&](ttt::intent::kind_t k, std::uint16_t slot, std::uint64_t id, std::uint64_t orig, std::uint32_t price,
                          std::uint32_t qty) {
        ttt::intent i{};
        i.kind = k;
        i.slot = slot;
        i.id = id;
        i.orig_id = orig;
        i.price = price;
        i.quantity = qty;
        ttt::order_message m;
        w.write(i, utc.now_ns(), m);
        return submit_order(*in, m.msg_type(), m.text(), -1);
    };
    using A = ttt::order_answer;
    struct row {
        const char* what;
        std::vector<A> expect;
    };
    std::vector<row> rows;
    const auto step = [&](const char* what, std::vector<A> expect) {
        const std::size_t from = got.size();
        const bool ok = until(from + expect.size());
        // nothing more within 50 ms
        const std::int64_t quiet = clock_now().mono_ns + 50'000'000;
        while (clock_now().mono_ns < quiet) pump();
        const std::vector<A> seen(got.begin() + static_cast<long>(from), got.end());
        std::string s;
        for (const A& a : seen) s += (s.empty() ? "" : ", ") + text(a);
        check(ok && seen == expect, std::string(what) + ": " + (s.empty() ? "nothing" : s));
    };

    const std::uint64_t a = 1u << 16, b = 2u << 16, c = 3u << 16, d = 4u << 16, f = (5u << 16) | 1, g = (6u << 16) | 1;
    send(ttt::intent::new_order, 0, a, 0, 100'000, 100);  // 10.0000
    step("D, ExecType New: accepted by its ClOrdID", {A{A::accepted, a}});
    send(ttt::intent::replace, 0, b, a, 100'100, 100);
    step("G, ExecType Replaced (after Pending Replace, not an answer): replaced by the new ClOrdID", {A{A::replaced, b}});
    send(ttt::intent::cancel, 0, c, b, 100'100, 100);
    step("F, ExecType Canceled (after Pending Cancel): cancelled by the cancel's ClOrdID", {A{A::cancelled, c}});
    send(ttt::intent::cancel, 0, d, b, 100'100, 100);
    step("F for an order already finished: refused by the gateway before it left, rejected by the cancel's ClOrdID", {A{A::rejected, d}});
    sim.seed("BBB", '2', 2'000'000'000, 4'000'000'000);  // 40 to sell at 20.0000
    send(ttt::intent::new_order, 1, f, 0, 200'000, 100);
    step("D crossing 40: accepted, then partly filled 40 at 20.0000 (LastQty, LastPx)", {A{A::accepted, f}, A{A::partly_filled, f, 200'000, 40}});
    sim.seed("BBB", '2', 2'000'000'000, 6'000'000'000);  // 60 more: the rest of it
    send(ttt::intent::replace, 1, g, f, 200'000, 100);
    step("G at the same price, then filled by the rest (LeavesQty 0)", {A{A::replaced, g}, A{A::filled, g, 200'000, 60}});
    check(unmapped == 0, "no slot unmapped (" + std::to_string(unmapped) + ")");
    check(sim.duplicates() == 0, "the venue saw no duplicate ClOrdID");
    stop = true;
    t.join();
}

// The rows behind the gateway the simulator does not produce: built as the
// gateway delivers them (the message as received; an order event).
void built_slots() {
    const auto msg = [](std::string fields) {
        for (char& ch : fields)
            if (ch == '|') ch = '\x01';
        app_slot s{};
        s.kind = app_slot::message;
        const std::string m = "8=FIX.4.4\x01" "9=99\x01" + fields + "10=000\x01";
        s.length = static_cast<std::uint16_t>(m.size());
        std::memcpy(s.data, m.data(), m.size());
        return s;
    };
    const auto event = [](std::uint8_t slot_kind, std::uint8_t kind, std::string_view id) {
        order_event e{};
        e.kind = static_cast<decltype(e.kind)>(kind);
        e.id_length = static_cast<std::uint8_t>(id.size());
        std::memcpy(e.cl_ord_id, id.data(), id.size());
        return event_slot(e, slot_kind);
    };
    using A = ttt::order_answer;
    struct c {
        const char* what;
        app_slot s;
        ttt::answer_read r;
        A a;
    };
    const c cases[] = {
        {"ExecType Rejected (8): rejected by its ClOrdID", msg("35=8|11=65536|17=X1|150=8|39=8|151=0|"), ttt::answer_read::answer, {A::rejected, 65536}},
        {"OrderCancelReject (9): rejected by its ClOrdID (the request's)", msg("35=9|11=131072|41=65536|39=0|434=1|"), ttt::answer_read::answer,
         {A::rejected, 131072}},
        {"ExecType Canceled, the venue's own: cancelled by the order's ClOrdID", msg("35=8|11=65536|17=X2|150=4|39=4|151=0|"),
         ttt::answer_read::answer, {A::cancelled, 65536}},
        {"ExecType Pending New: nothing to answer", msg("35=8|11=65536|17=X3|150=A|39=A|151=100|"), ttt::answer_read::none, {}},
        {"ExecType Trade Cancel (H): unmapped, counted", msg("35=8|11=65536|17=X4|150=H|39=0|151=100|"), ttt::answer_read::unmapped, {}},
        {"a ClOrdID not ours: unmapped", msg("35=8|11=X-1|17=X5|150=0|39=0|151=100|"), ttt::answer_read::unmapped, {}},
        {"not sent (the gateway's D5): rejected by its ClOrdID", event(app_slot::order_state, order_event::not_sent, "196608"),
         ttt::answer_read::answer, {A::rejected, 196608}},
        {"refused by the gateway: rejected by the request's ClOrdID", event(app_slot::refused, order_event::refused, "262144"),
         ttt::answer_read::answer, {A::rejected, 262144}},
        {"an order's state after its message: nothing more", event(app_slot::order_state, order_event::state, "65536"), ttt::answer_read::none, {}},
        {"replayed: unmapped (a fresh session has none, D6)", [&] { app_slot s = msg("35=8|11=1|150=0|"); s.kind = app_slot::replayed; return s; }(),
         ttt::answer_read::unmapped, {}},
    };
    for (const c& x : cases) {
        A a{};
        const ttt::answer_read r = ttt::read_answer(x.s, a);
        check(r == x.r && (r != ttt::answer_read::answer || a == x.a), x.what);
    }
}

}  // namespace

int main() {
    transact_time();
    built_slots();
    through_the_gateway();
    return ttt::test::result();
}
