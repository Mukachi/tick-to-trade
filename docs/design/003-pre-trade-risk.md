# 003 — Pre-trade risk

**Status:** Agreed · **Updated:** 2026-10-31

## Proposal

Component 4 of note 001: every intent of the strategy (note 002) passes the risk checks or is
refused before it reaches the gateway, and the risk state follows every order from the moment it
is sent to its last answer. The checks are note 001's D3, taken from the public rules' text.

It does:

- the checks of RTS 6 Article 15 on order entry, its execution throttle, an instrument permission
  list, and SEC Rule 15c3-5's credit threshold on exposure from open orders and fills, each order
  counted as it is sent (Art. 15(2));
- refusals by reason, counted; a refused intent answered to the strategy as `rejected`, at once;
- the kill switch (RTS 6 Art. 12): no new order or replace passes, and every open order is
  cancelled; engaged by the operator or by the execution throttle, re-enabled only by the operator;
- a reference implementation, written for clarity, the oracle and the baseline;
- limits from a configuration file.

It does not:

- override a refusal (Art. 15(6)'s procedure for submitting a blocked order: D6);
- monitor in real time for disorderly trading (Art. 16), or give the operator a way in: note 004
  (the program, its monitoring block and the kill command);
- keep positions across days, margin, or check after the trade (note 001);
- check what the gateway already checks for the session (ClOrdID uniqueness, the order state
  machine): the gateway's own refusals come back as `rejected` and release what they held.

## Design

**What the rules ask, and the check for each** (text from legislation.gov.uk's copy of RTS 6,
up to date to 30 October 2026, revocation under FSMA 2023 not yet applied; 15c3-5 from Cornell
LII's copy of 17 CFR 240.15c3-5):

| Rule | Its words, in short | The check |
|---|---|---|
| Art. 15(1)(a) | price collars, order by order and over a specified period | **collar**: a buy's price within a band of the instrument's best ask, and within a band of a reference price fixed at the start of each period (D2) |
| Art. 15(1)(b); 15c3-5 (c)(1)(ii) | maximum order value; erroneous orders by price or size | **order value**: price x quantity at most a limit |
| Art. 15(1)(c); 15c3-5 (c)(1)(ii) | maximum order volume | **order volume**: quantity at most a limit |
| Art. 15(1)(d) | maximum messages: submission, modification or cancellation | **messages**: news, replaces and cancels counted in a sliding window; news and replaces refused at the limit, cancels counted and never refused |
| 15c3-5 (c)(1)(ii) | duplicative orders | **duplicate**: a new order for an instrument that already has an order open or in flight |
| Art. 15(2) | all orders sent included in the limits immediately | every passed request counted at once, before any answer (D3) |
| Art. 15(3) | after a set number of repeated executions, disabled until re-enabled by a designated person | **execution throttle**: fills counted in a sliding window; at the limit the kill switch engages (D4) |
| Art. 15(4), (5); 15c3-5 (c)(1)(i) | market and credit limits; block orders compromising risk thresholds, by instrument and firm | **exposure**: per instrument, open buy value plus position value at most a limit; for the firm, the sum at most a limit (D5) |
| Art. 15(5); 15c3-5 (c)(2)(ii) | orders in an instrument the trader may not trade | **permission**: the instrument on the configured list |
| note 001, D3 | no order on a book that cannot be trusted | **book usable**: the instrument's last book event usable (note 002's rule) |
| note 001, S2 | none while the gateway is not logged on | **session**: the gateway logged on (an input the program sets, note 004) |
| Art. 12 | cancel any or all unexecuted orders at once | **kill switch** |

Checks run in a fixed order (kill, session, permission, book usable, duplicate, volume, value,
collar, messages, exposure), and a refusal names the first that failed. Cancels pass every
check: they only lower exposure, and a kill must be able to send them.

```mermaid
flowchart LR
  T[strategy] -->|intent| R{risk checks}
  R -->|passed: counted at once| G[gateway]
  R -->|refused: rejected, by reason| T
  G -->|answers| R
  R -->|answers| T
  E[book events] --> R
  K[operator: kill / re-enable] --> R
```

**The risk state.** Per instrument: the order open (its price and quantity), the request in
flight, the position (shares bought and filled, and their cost), the last book event. For the
firm: the exposure summed, the message window, the fill window, the kill switch. Answers move
it: accepted (nothing changes: it was counted at send), replaced (the new price and quantity
replace the old), cancelled or rejected (released), filled and partly filled (open value to
position, at the fill's price; note 004 gives the fill's price and quantity with the answer, so
`order_answer` gains them).

**Prices and values.** ITCH prices (four implied decimals) and shares, values in 1e-4 dollars
in 64 bits (a $100,000 order is 10^9: room to spare); bands in basis points.

**Time.** The windows (messages, fills, the collar's periods) run on the ITCH timestamp of the
last book event seen (D1), so a recorded day replayed gives the same verdicts (note 001, S3).

**The reference implementation**: the same checks over standard containers, recomputing exposure
from every order and fill kept (no running sums), so a sum kept wrong in the fast one shows.

**Decisions** (each with what is proposed):

- **D1. The windows' clock.** *Proposed (A):* the ITCH timestamp of the last book event (market
  time): deterministic on replay; at the day's own pace it is real time. *Alternative (B):*
  `CLOCK_MONOTONIC`: real time always, but a replay at another pace gives other verdicts and
  note 001's S3 holds only with the limits loose.
- **D2. The collar over a period.** *Proposed (A):* each instrument's reference price is its best
  bid at the first usable event of each period (default one minute); an order's price more than
  the band (default 10%) from it is refused, so a market moving fast is followed only a band a
  minute. *Alternative (B):* order by order only (the best ask band), the period left to the
  venue's own limit-up limit-down: weaker than Art. 15(1)(a)'s "over a specified period".
- **D3. A replace in flight.** *Proposed (A):* counted at both prices until answered (the old
  order can still fill while the new is being placed): the worst case, as Art. 15(2) asks.
  *Alternative (B):* the larger of the two: less refused, but a fill of the old order during a
  replace that then succeeds is not covered.
- **D4. The execution throttle.** *Proposed (A):* fills (whole or part) counted in a sliding
  window (default 1,000 a minute); at the limit the kill switch engages, re-enabled only by the
  operator. Art. 15(3)'s "number of times an algorithmic trading strategy has been applied" read
  as executions, its own next words. *Alternative (B):* intents counted instead: that is the
  messages limit again.
- **D5. Market and credit limits.** *Proposed (A):* per instrument, open buy value plus position
  value (shares held at cost) at most a limit; for the firm the sum at most a limit: the
  strategy only buys, so exposure only grows with fills, and the day shows the limit stopping
  new orders. *Alternative (B):* the firm's limit only: Art. 15(5) asks for instrument-level
  controls "where appropriate".
- **D6. Overrides (Art. 15(6)).** *Proposed (A):* none: every refusal is final, stated as out of
  scope (an override is a procedure with a designated person, not code). *Alternative (B):* a
  one-shot override command for a named request: code for a procedure the repo cannot show.
- **D7. The gateway's own throttle.** *Proposed (A):* off: the messages limit here is the one
  limit, refusing with a reason the strategy is told at once; the gateway's would refuse later
  with a retry time. *Alternative (B):* both on, the gateway's looser, as a second line.

## Spec

- S1. Each check refuses the intents built to break it, by the reason named, counted; each
  refused intent is answered `rejected` to the strategy and reaches no gateway; cancels pass
  every check, the kill switch's included.
- S2. Counting at send: from the moment a request passes, the exposure, message and duplicate
  checks count it, before any answer; rejection or cancellation releases it, a fill moves it to
  position; a replace in flight counts both prices (D3). Tested by sequences of intents and answers.
- S3. The kill switch: once engaged (by the operator or by D4's throttle) no new order or replace
  passes, a cancel goes for every open order and request in flight, and only the operator
  re-enables it.
- S4. Over the sample day (D5's 100 instruments, note 002's model venue, limits set so that every
  check refuses some), the fast checks and the reference implementation give the same verdict
  for every intent and the same exposure after every answer; and a random run of intents and
  answers likewise.
- S5. Over the same day, every intent passed is checked again after the run against the limits
  as they stood when it was sent, from the log alone: every order the venue receives passed
  every check (note 001's S1).
- S6. Checking an intent and taking an answer allocate nothing and make no system call (the
  compiled call graph checked).
- S7. The cost of a check, and of the risk thread's work over the day, measured beside the
  reference implementation and published via `run.py report`.

## Tasks

- [ ] The limits and their configuration file; `order_answer` with a fill's price and quantity
- [ ] The checks and the risk state (D1–D5, D7), the kill switch
- [ ] The reference implementation
- [ ] Tests S1–S6
- [ ] The benchmark (S7)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-31 | Draft | Written for review. Checked: RTS 6 Articles 12, 15 and 16 on legislation.gov.uk (up to date to 30 October 2026; revocation by FSMA 2023 listed, not yet applied; EUR-Lex gave no text to compare); 17 CFR 240.15c3-5 (c) on Cornell LII (eCFR redirected). Art. 15(3) is not a rate limit: it disables the system after a number of executions until a designated person re-enables it (D4). 15c3-5 (c)(1)(ii) also asks for duplicative orders to be refused (the duplicate check). The gateway's order book already throttles messages if configured (its note 007, D4): D7 |
| 2026-10-31 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 market time for the windows; D2 a reference price a period (one minute, 10% band); D3 a replace in flight counted at both prices; D4 fills in a sliding window engage the kill switch; D5 instrument and firm exposure limits; D6 no override; D7 the gateway's throttle off |
