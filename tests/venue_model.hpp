// A model venue for the strategy's tests (note 002, S2): after each event it
// may answer the oldest request (a third of the time), mostly as asked, a
// tenth rejected, a replace or cancel of an order no longer resting rejected;
// and now and then fills (wholly or in part) a resting order. Seeded, so the
// same intents always get the same answers: two strategies that agree are
// answered alike. Not note 001's venue (order_venue, in note 004).
#pragma once

#include <ttt/strategy.hpp>

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

namespace ttt::test {

template <class S>
struct venue {
    std::mt19937_64 rng;
    std::deque<ttt::intent> waiting;
    std::vector<std::uint64_t> resting;  // per slot
    venue(std::size_t slots, std::uint64_t seed) : rng(seed), resting(slots, 0) {}
    void intent(const ttt::intent& i) { waiting.push_back(i); }
    void step(S& s) {
        if (!waiting.empty() && rng() % 3 == 0) {
            const ttt::intent i = waiting.front();
            waiting.pop_front();
            const bool reject = rng() % 10 == 0;
            if (reject) {
                s.answer(order_answer{order_answer::rejected, i.id});
                return;
            }
            switch (i.kind) {
                case ttt::intent::new_order: resting[i.slot] = i.id; s.answer(order_answer{order_answer::accepted, i.id}); break;
                case ttt::intent::replace:
                    if (resting[i.slot] != i.orig_id) s.answer(order_answer{order_answer::rejected, i.id});
                    else resting[i.slot] = i.id, s.answer(order_answer{order_answer::replaced, i.id});
                    break;
                case ttt::intent::cancel:
                    if (resting[i.slot] != i.orig_id) s.answer(order_answer{order_answer::rejected, i.id});
                    else resting[i.slot] = 0, s.answer(order_answer{order_answer::cancelled, i.id});
                    break;
            }
        }
        if (rng() % 50 == 0) {
            const std::size_t slot = rng() % resting.size();
            if (resting[slot]) {
                const std::uint64_t id = resting[slot];
                if (rng() % 2) resting[slot] = 0, s.answer(order_answer{order_answer::filled, id});
                else s.answer(order_answer{order_answer::partly_filled, id});
            }
        }
    }
};

template <class S>
struct venue_out {
    venue<S>* v;
    std::vector<ttt::intent> log;
    void intent(const ttt::intent& i) {
        log.push_back(i);
        v->intent(i);
    }
};

}  // namespace ttt::test
