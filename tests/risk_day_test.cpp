// Note 003 over the sample day (S4, S5), label `data`: needs data/, so CI
// does not run it. Every message of the day into the handler on the default
// book with the strategy's sink (note 002, D5's 100 instruments); each book
// event to the strategy and to both the risk checks and the reference
// implementation; the strategy's intents through the checks to note 002's
// model venue, every intent and every answer given to both, the strategy
// told what the checks say. The limits are set so that the day reaches as
// many checks as the strategy can: a value limit below the dearest
// instruments' round lots, a permission list without the ten quietest, a
// tight period collar and messages limit, instrument and firm exposure the
// model venue's fills reach, a fill throttle (its kills re-enabled by the
// operator five minutes on); the session down 09:30:10-09:30:40; the
// operator's kill 09:45:00, re-enabled 09:50:00.
//   S4: after every message, the same intents sent, refusals, answers to the
//   strategy and counts; every 100,000 messages and at the end, every
//   instrument's exposure and position, and the firm's.
//   S5: from the log alone (book events, the operator's actions, every intent
//   with its verdict, every answer), a fresh reference implementation replays
//   the day and passes every intent the checks passed, and refuses every one
//   they refused, by the same reason.
//
// Usage: risk_day_test [day.gz] [instruments.txt]
#include "check.hpp"
#include "venue_model.hpp"

#include "day_file.hpp"  // tools/

#include <ttt/reference_risk.hpp>
#include <ttt/risk.hpp>
#include <ttt/sink.hpp>
#include <ttt/strategy.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace ttt;
using ttt::test::check;

struct collect {
    std::vector<book_event> v;
    void push(const book_event& e) { v.push_back(e); }
};
struct sent {
    std::vector<ttt::intent> v;
    void send(const ttt::intent& i) { v.push_back(i); }
};

// The day's log, for S5.
enum class act : std::uint8_t { session_up, session_down, kill, reenable };
struct op {
    act what;
};
struct verdict {
    ttt::intent i;
    refusal r;  // none: passed (a cancel always)
};
using record = std::variant<book_event, op, verdict, order_answer>;

// The strategy's intents go to both checks, logged with the fast checks' verdict.
struct to_risk;
struct strategy_out {
    to_risk* r;
    void intent(const ttt::intent& i);
};
struct to_risk {
    risk<sent>& fast;
    reference_risk<sent>& ref;
    std::vector<record>& log;
    std::string& problem;
    strategy<strategy_out>* s = nullptr;
    void intent(const ttt::intent& i) {
        const auto before = fast.counts().refused;
        fast.intent(i);
        ref.intent(i);
        refusal r = refusal::none;
        for (int k = 0; k < refusal_kinds; ++k)
            if (fast.counts().refused[k] != before[k]) r = static_cast<refusal>(k);
        log.push_back(verdict{i, r});
    }
    // Refusals, to the strategy (which may send again)
    void settle() {
        order_answer a, b;
        for (;;) {
            const bool gf = fast.take_refusal(a), gr = ref.take_refusal(b);
            if (gf != gr || (gf && !(a == b))) problem = "S4: the refusals differ";
            if (!gf || !gr) return;
            s->answer(a);
        }
    }
};
void strategy_out::intent(const ttt::intent& i) { r->intent(i); }

// The venue's answers to both checks; the strategy told what the checks say.
struct router {
    to_risk& tr;
    void answer(const order_answer& a) {
        tr.log.push_back(a);
        order_answer f, r;
        const bool gf = tr.fast.answer(a, f), gr = tr.ref.answer(a, r);
        if (gf != gr || (gf && !(f == r))) tr.problem = "S4: the answers to the strategy differ";
        if (gf) tr.s->answer(f);
        tr.settle();
    }
};

constexpr std::uint64_t at(int h, int m, int sec = 0) {
    return (static_cast<std::uint64_t>(h) * 3600 + static_cast<std::uint64_t>(m) * 60 + static_cast<std::uint64_t>(sec)) * 1'000'000'000ull;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : TTT_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::string list = argc > 2 ? argv[2] : TTT_CONFIG_DIR "/open-100.txt";
    std::vector<std::string> names;
    {
        std::ifstream f(list);
        for (std::string s; std::getline(f, s);)
            if (!s.empty()) names.push_back(s);
    }
    const std::vector<std::string_view> traded(names.begin(), names.end());
    check(traded.size() == 100, list + ": " + std::to_string(traded.size()) + " instruments");
    day_file day(path);
    if (!day.open()) {
        check(false, path + ": cannot read (the feed handler's tools/fetch_day.py fetches it)");
        return ttt::test::result();
    }
    const auto t0 = std::chrono::steady_clock::now();

    limits lim;
    lim.max_value = 300'000'000;  // $30,000: a round lot above $300 refused
    lim.period_collar_bps = 30;
    lim.max_messages = 40;
    lim.max_fills = 40;
    lim.fills_window_ns = 60'000'000'000;
    lim.instrument_exposure = 1'000'000'000;  // $100,000
    lim.firm_exposure = 50'000'000'000;       // $5 million
    for (std::size_t k = 0; k < 90; ++k) lim.permitted.push_back(names[k]);  // the last ten (the quietest) not permitted

    instruments ins(traded);
    auto book = std::make_unique<itchfh::default_book>();
    collect events;
    strategy_feed<collect> feed(*book, ins, events);
    feed.feed(itchfh::feed_state::live, 1);

    sent fo, ro;
    risk<sent> fast(ins, lim, fo);
    reference_risk<sent> ref(ins, lim, ro);
    std::vector<record> log;
    log.reserve(1u << 26);

    std::string problem;
    to_risk tr{fast, ref, log, problem};
    strategy_out so{&tr};
    strategy<strategy_out> strat(traded.size(), so, {1});
    tr.s = &strat;
    router rt{tr};
    test::venue<router> venue(traded.size(), 7);

    std::uint64_t n = 0, nevents = 0, compared = 0;
    bool session_back = false;
    std::uint64_t throttle_since = 0;
    bool up = false, down = false, killed = false, reenabled = false;
    const auto act_now = [&](act w) {
        log.push_back(op{w});
        switch (w) {
            case act::session_up: fast.session(true), ref.session(true); break;
            case act::session_down: fast.session(false), ref.session(false); break;
            case act::kill: fast.kill(), ref.kill(); break;
            case act::reenable: fast.reenable(), ref.reenable(); break;
        }
    };
    const auto operator_at = [&](std::uint64_t ts) {
        if (!up) up = true, act_now(act::session_up);
        if (!down && ts >= at(9, 30, 10)) down = true, act_now(act::session_down);
        if (down && ts >= at(9, 30, 40) && !session_back) session_back = true, act_now(act::session_up);
        if (!killed && ts >= at(9, 45)) killed = true, act_now(act::kill);
        if (killed && !reenabled && ts >= at(9, 50)) reenabled = true, act_now(act::reenable);
        // The fill throttle's kills (any time but the operator's own): re-enabled five minutes on
        if (fast.killed() && !(killed && !reenabled)) {
            if (!throttle_since) throttle_since = ts;
            else if (ts - throttle_since >= at(0, 5)) throttle_since = 0, act_now(act::reenable);
        } else {
            throttle_since = 0;
        }
    };
    const auto compare_state = [&](const std::string& when) {
        if (!problem.empty()) return;
        if (fast.firm_exposure() != ref.firm_exposure()) problem = "S4: " + when + ": firm exposure differs";
        for (std::size_t s = 0; s < ins.size() && problem.empty(); ++s)
            if (fast.exposure(s) != ref.exposure(s) || fast.position(s) != ref.position(s))
                problem = "S4: " + when + ": instrument " + std::to_string(s) + "'s exposure or position differs";
    };
    itchfh::message m;
    std::uint64_t max_firm = 0;
    while (day.next(m) && problem.empty()) {
        ++n;
        feed.on(day.sequence(), m);
        operator_at(m.timestamp_ns);
        for (const book_event& e : events.v) {
            log.push_back(e);
            fast.on(e);
            ref.on(e);
            strat.on(e);
            tr.settle();
            venue.step(rt);
        }
        nevents += events.v.size();
        events.v.clear();
        // What each sent: the same, and the fast checks' sends go to the venue
        if (fo.v.size() != ro.v.size() || !std::equal(fo.v.begin() + static_cast<std::ptrdiff_t>(compared), fo.v.end(), ro.v.begin() + static_cast<std::ptrdiff_t>(compared)))
            problem = "S4: message " + std::to_string(n) + ": the intents sent differ";
        for (std::size_t k = compared; k < fo.v.size(); ++k) venue.intent(fo.v[k]);
        compared = fo.v.size();
        if (!(fast.counts() == ref.counts()) || fast.killed() != ref.killed()) problem = "S4: message " + std::to_string(n) + ": the counts differ";
        max_firm = std::max(max_firm, fast.firm_exposure());
        if (n % 100'000 == 0) compare_state("message " + std::to_string(n));
    }
    compare_state("the end");
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const risk_counts& c = fast.counts();
    std::string reasons;
    for (int k = 1; k < refusal_kinds; ++k) reasons += std::string(reasons.empty() ? "" : ", ") + name(static_cast<refusal>(k)) + " " + std::to_string(c.refused[k]);
    check(n > 400'000'000, std::to_string(n) + " messages, " + std::to_string(nevents) + " book events (" + std::to_string(static_cast<int>(secs)) + " s)");
    check(problem.empty(), "S4 over the day: the checks and the reference implementation agree on every send, refusal, answer and count after every message, "
                           "every exposure every 100,000 messages (" + std::to_string(c.passed) + " passed, " + std::to_string(c.cancels) + " cancels, " +
                           std::to_string(c.fills) + " fills, " + std::to_string(c.kill_cancels) + " kill cancels)" + (problem.empty() ? "" : ": " + problem));
    check(c.refused[static_cast<int>(refusal::killed)] && c.refused[static_cast<int>(refusal::session)] && c.refused[static_cast<int>(refusal::permission)] &&
              c.refused[static_cast<int>(refusal::value)] && c.refused[static_cast<int>(refusal::period_collar)] && c.refused[static_cast<int>(refusal::messages)] &&
              c.refused[static_cast<int>(refusal::instrument_exposure)] && c.refused[static_cast<int>(refusal::firm_exposure)] && c.kills >= 2 && c.kill_cancels > 0,
          "the day reached the checks the strategy can reach: " + reasons + "; kill switch engaged " + std::to_string(c.kills) +
              " times; the firm's exposure peaked at $" + std::to_string(max_firm / 10'000));
    check(fast.firm_exposure() <= lim.firm_exposure && max_firm <= lim.firm_exposure, "the firm's exposure never past its limit");

    // S5: the log alone, through a fresh reference implementation
    sent ao;
    reference_risk<sent> audit(ins, lim, ao);
    std::uint64_t checked = 0, wrong = 0, passed = 0;
    std::string first;
    for (const record& r : log) {
        if (const auto* e = std::get_if<book_event>(&r)) audit.on(*e);
        else if (const auto* o = std::get_if<op>(&r)) {
            if (o->what == act::session_up) audit.session(true);
            else if (o->what == act::session_down) audit.session(false);
            else if (o->what == act::kill) audit.kill();
            else audit.reenable();
        } else if (const auto* v = std::get_if<verdict>(&r)) {
            const auto before = audit.counts().refused;
            audit.intent(v->i);
            refusal got = refusal::none;
            for (int k = 0; k < refusal_kinds; ++k)
                if (audit.counts().refused[k] != before[k]) got = static_cast<refusal>(k);
            order_answer drop;
            while (audit.take_refusal(drop)) {
            }
            ++checked;
            passed += v->r == refusal::none;
            if (got != v->r && !wrong++) first = std::string("intent ") + std::to_string(checked) + ": logged " + name(v->r) + ", replayed " + name(got);
        } else {
            order_answer to;
            audit.answer(std::get<order_answer>(r), to);
        }
    }
    check(wrong == 0 && ao.v == fo.v, "S5: from the log alone, " + std::to_string(checked) + " intents rechecked: every one passed (" + std::to_string(passed) +
                                          ") passes again and every refusal is the same; the venue received exactly what passed" + (wrong ? " (" + first + ")" : ""));
    return ttt::test::result();
}
