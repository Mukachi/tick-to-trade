// The reference risk checks (note 003, S4): note 003's checks written a
// second time for clarity, over standard containers, keeping everything
// (every order, every fill, every message's and fill's time) and computing
// each figure from it when asked: exposure summed from the orders and fills
// kept, the windows counted from every time kept. No running sums, so a sum
// the fast checks keep wrong shows. Same calls, same answers.
// Design: docs/design/003-pre-trade-risk.md
#pragma once

#include <ttt/events.hpp>
#include <ttt/limits.hpp>
#include <ttt/risk.hpp>
#include <ttt/strategy.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace ttt {

template <class Out>
class reference_risk {
public:
    reference_risk(const instruments& ins, const limits& l, Out& out) : ins_(ins), lim_(l), out_(out) {
        for (std::size_t i = 0; i < ins.size(); ++i) {
            bool ok = l.permitted.empty();
            for (const std::string& p : l.permitted) ok = ok || to_symbol(p) == ins.symbol(i);
            if (ok) permitted_.insert(static_cast<std::uint16_t>(i));
        }
    }

    void on(const book_event& e) {
        now_ = std::max(now_, e.timestamp_ns);
        last_[e.slot] = e;
        if (usable(e)) {
            const std::uint64_t period = now_ / lim_.period_ns;
            if (!reference_.contains(e.slot) || reference_[e.slot].first != period) reference_[e.slot] = {period, e.bid_price};
        }
    }
    void session(bool up) { session_ = up; }
    void kill() {
        if (!killed_) ++counts_.kills;
        killed_ = true;
        for (std::size_t i = 0; i < ins_.size(); ++i) cancel_for_kill(static_cast<std::uint16_t>(i));
    }
    void reenable() {
        killed_ = false;
        fills_from_ = fill_times_.size();
    }

    void intent(const ttt::intent& i) {
        if (i.kind == intent::cancel) {
            ++counts_.cancels;
            message_times_.push_back(now_);
            cancels_[i.slot] = i.id;
            out_.send(i);
            return;
        }
        const refusal r = verdict(i);
        if (r != refusal::none) {
            ++counts_.refused[static_cast<int>(r)];
            refusals_.push_back(order_answer{order_answer::rejected, i.id});
            return;
        }
        ++counts_.passed;
        message_times_.push_back(now_);
        requests_[i.slot] = i;
        out_.send(i);
    }
    bool take_refusal(order_answer& a) {
        if (refusals_.empty()) return false;
        a = refusals_.front();
        refusals_.pop_front();
        return true;
    }

    bool answer(const order_answer& a, order_answer& to) {
        to = a;
        const std::uint16_t slot = slot_of(a.id);
        if (slot >= ins_.size()) {
            ++counts_.unknown_answers;
            return true;
        }
        // The risk's own cancel
        if (kill_cancels_.contains(slot) && kill_cancels_[slot] == a.id) {
            kill_cancels_.erase(slot);
            const std::optional<std::uint64_t> o = open_id(slot);
            if (a.kind == order_answer::cancelled && o) {
                to = order_answer{order_answer::cancelled, *o};
                orders_.erase(*o);
                return true;
            }
            cancel_for_kill(slot);
            return false;
        }
        const std::optional<ttt::intent> req = requests_.contains(slot) ? std::optional<ttt::intent>(requests_[slot]) : std::nullopt;
        const std::optional<std::uint64_t> open = open_id(slot);
        bool is_req = req && req->id == a.id;
        bool is_open = open && *open == a.id;
        const bool a_fill = a.kind == order_answer::filled || a.kind == order_answer::partly_filled;
        if (is_req && req->kind == intent::replace && a_fill) {  // the replace was done
            replace_order(slot, *req);
            requests_.erase(slot);
            is_req = false;
            is_open = true;
        }
        if (a.kind == order_answer::accepted && is_req && req->kind == intent::new_order) {
            orders_[req->id] = order_t{slot, req->price, req->quantity};
            requests_.erase(slot);
            cancel_for_kill(slot);
        } else if (a.kind == order_answer::replaced && is_req && req->kind == intent::replace) {
            replace_order(slot, *req);
            requests_.erase(slot);
            cancel_for_kill(slot);
        } else if (a.kind == order_answer::rejected && is_req) {
            requests_.erase(slot);
        } else if (a.kind == order_answer::rejected && cancels_.contains(slot) && cancels_[slot] == a.id) {
            cancels_.erase(slot);
        } else if (a.kind == order_answer::cancelled && cancels_.contains(slot) && cancels_[slot] == a.id) {
            cancels_.erase(slot);
            if (open) orders_.erase(*open);
        } else if (a.kind == order_answer::cancelled && (is_open || is_req)) {
            if (open) orders_.erase(*open);
            if (is_req) requests_.erase(slot);
        } else if (a_fill && (is_open || is_req)) {
            ++counts_.fills;
            fills_[slot].push_back({a.quantity, a.price});
            fill_times_.push_back(now_);
            if (is_open) {
                order_t& o = orders_[a.id];
                o.leaves -= std::min(a.quantity, o.leaves);
                if (a.kind == order_answer::filled) orders_.erase(a.id);
            } else {
                ttt::intent& r = requests_[slot];
                r.quantity -= std::min(a.quantity, r.quantity);
                if (a.kind == order_answer::filled) requests_.erase(slot);
            }
            if (!killed_ && fills_in_window() >= lim_.max_fills) kill();
        }
        return true;
    }

    const risk_counts& counts() const { return counts_; }
    bool killed() const { return killed_; }
    std::uint64_t exposure(std::size_t slot) const {
        std::uint64_t e = 0;
        if (fills_.contains(static_cast<std::uint16_t>(slot)))
            for (const auto& [q, p] : fills_.at(static_cast<std::uint16_t>(slot))) e += std::uint64_t{q} * p;
        std::uint32_t leaves = 0;
        for (const auto& [id, o] : orders_)
            if (o.slot == slot) e += std::uint64_t{o.price} * o.leaves, leaves = o.leaves;
        if (requests_.contains(static_cast<std::uint16_t>(slot))) {
            const ttt::intent& r = requests_.at(static_cast<std::uint16_t>(slot));
            if (r.kind == intent::new_order) e += std::uint64_t{r.price} * r.quantity;
            if (r.kind == intent::replace) e += std::uint64_t{r.price} * leaves;
        }
        return e;
    }
    std::uint64_t firm_exposure() const {
        std::uint64_t e = 0;
        for (std::size_t s = 0; s < ins_.size(); ++s) e += exposure(s);
        return e;
    }
    std::uint64_t position(std::size_t slot) const {
        std::uint64_t n = 0;
        if (fills_.contains(static_cast<std::uint16_t>(slot)))
            for (const auto& [q, p] : fills_.at(static_cast<std::uint16_t>(slot))) n += q;
        return n;
    }

private:
    struct order_t {
        std::uint16_t slot;
        std::uint32_t price, leaves;
    };

    std::optional<std::uint64_t> open_id(std::uint16_t slot) const {
        for (const auto& [id, o] : orders_)
            if (o.slot == slot) return id;
        return std::nullopt;
    }
    void replace_order(std::uint16_t slot, const ttt::intent& r) {
        const std::optional<std::uint64_t> o = open_id(slot);
        if (!o) {  // gone by our account, but the venue says it lives: its whole quantity
            orders_[r.id] = order_t{slot, r.price, r.quantity};
            return;
        }
        order_t moved = orders_[*o];
        moved.price = r.price;
        orders_.erase(*o);
        orders_[r.id] = moved;
    }
    // Times are kept in order (market time never goes back): counted from the newest until one is outside the window.
    std::uint64_t in_window(const std::vector<std::uint64_t>& times, std::size_t from, std::uint64_t window) const {
        std::uint64_t n = 0;
        for (std::size_t k = times.size(); k > from && times[k - 1] + window > now_; --k) ++n;
        return n;
    }
    std::uint64_t fills_in_window() const { return in_window(fill_times_, fills_from_, lim_.fills_window_ns); }

    refusal verdict(const ttt::intent& i) const {
        if (killed_) return refusal::killed;
        if (!session_) return refusal::session;
        if (!permitted_.contains(i.slot)) return refusal::permission;
        const auto ev = last_.find(i.slot);
        if (ev == last_.end() || !usable(ev->second)) return refusal::unusable;
        if (requests_.contains(i.slot) || cancels_.contains(i.slot) || (i.kind == intent::new_order && open_id(i.slot))) return refusal::duplicate;
        if (i.quantity > lim_.max_volume) return refusal::volume;
        const std::uint64_t price = i.price, bid = ev->second.bid_price, ask = ev->second.ask_price, band = lim_.collar_bps;
        if (price * i.quantity > lim_.max_value) return refusal::value;
        // In basis points, exactly: above the ask by more than the band, or below the bid by more
        if ((ask > 0 && price * 10'000 > ask * (10'000 + band)) || (bid > 0 && price * 10'000 < bid * (10'000 - std::min<std::uint64_t>(band, 10'000))))
            return refusal::collar;
        if (const auto r = reference_.find(i.slot); r != reference_.end() && r->second.second > 0) {
            const std::uint64_t ref = r->second.second, off = price > ref ? price - ref : ref - price;
            if (off * 10'000 > ref * lim_.period_collar_bps) return refusal::period_collar;
        }
        if (in_window(message_times_, 0, lim_.messages_window_ns) >= lim_.max_messages) return refusal::messages;
        std::uint32_t leaves = 0;
        for (const auto& [id, o] : orders_)
            if (o.slot == i.slot) leaves = o.leaves;
        const std::uint64_t add = std::uint64_t{i.price} * (i.kind == intent::new_order ? i.quantity : leaves);
        if (exposure(i.slot) + add > lim_.instrument_exposure) return refusal::instrument_exposure;
        if (firm_exposure() + add > lim_.firm_exposure) return refusal::firm_exposure;
        return refusal::none;
    }
    void cancel_for_kill(std::uint16_t slot) {
        const std::optional<std::uint64_t> o = open_id(slot);
        if (!killed_ || !o || kill_cancels_.contains(slot)) return;
        const std::uint64_t id = kill_bit | next_kill_++ << 16 | slot;
        kill_cancels_[slot] = id;
        ++counts_.kill_cancels;
        message_times_.push_back(now_);
        const order_t& ord = orders_.at(*o);
        out_.send(ttt::intent{intent::cancel, slot, ins_.locate(slot), id, *o, ord.price, ord.leaves, 0});
    }

    const instruments& ins_;
    limits lim_;
    Out& out_;
    std::set<std::uint16_t> permitted_;
    std::map<std::uint16_t, book_event> last_;
    std::map<std::uint16_t, std::pair<std::uint64_t, std::uint32_t>> reference_;  // period, price
    std::map<std::uint64_t, order_t> orders_;
    std::map<std::uint16_t, ttt::intent> requests_;
    std::map<std::uint16_t, std::uint64_t> kill_cancels_, cancels_;  // the risk's own cancels; the strategy's
    std::map<std::uint16_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> fills_;  // shares, price
    std::vector<std::uint64_t> message_times_, fill_times_;
    std::size_t fills_from_ = 0;
    std::uint64_t now_ = 0, next_kill_ = 1;
    bool session_ = false, killed_ = false;
    risk_counts counts_;
    std::deque<order_answer> refusals_;
};

}  // namespace ttt
