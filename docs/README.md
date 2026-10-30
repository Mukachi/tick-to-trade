# Documentation

Every component gets a design note before any code is written. Decisions with real alternatives
get an ADR.

- **Design notes** (`design/`): one file per component, sections Proposal, Design, Spec, Tasks,
  Review. Status moves Draft → Agreed → Built → Measured. Two review points: Draft → Agreed
  (approved before code) and Built → Measured (results checked against the Spec).
- **Open items** ([open.md](open.md)): everything a Review leaves open, each with a decision
  (fix, limit or parked). A note does not reach Measured while an item it raised has none.
- **ADRs** (`adr/`): Context, Decision, Consequences, Status. Only for choices a reviewer might
  question.

Only Spec and Review are mandatory. Any section can be one line.

## Design notes

| Note | Component | Status |
|---|---|---|
| [001](design/001-scope.md) | Scope and plan | Agreed |
| [002](design/002-book-events-strategy.md) | Book events and the strategy | Draft |
