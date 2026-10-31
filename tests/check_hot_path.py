"""S5 of note 002 and S6 of note 003, checked in the compiled code, as the feed handler's and the gateway's checks.
Design: docs/design/002-book-events-strategy.md

Usage: check_hot_path.py <objdump> <hot_path_subject binary>

Each subject is checked with everything it reaches: every function called,
transitively, may call only memory copies, moves, sets and comparisons and the
stack protector's abort, never through a pointer, and contains no syscall
instruction. The subjects: the feed thread's work for a message (the feed
handler's handler on the default book, the strategy's sink, the push into D4's
ring), a change of the feed's state, its tick; the adapter's for an update;
the strategy's for an event and for an answer; the strategy thread's pop; the
risk checks' for an intent, an answer, a book event, a refusal taken, the kill.
"""
import re
import subprocess
import sys

SUBJECTS = ["ttt_feed_message", "ttt_feed_state", "ttt_feed_tick", "ttt_adapter_update", "ttt_strategy_event",
            "ttt_strategy_answer", "ttt_ring_pop", "ttt_risk_intent", "ttt_risk_answer", "ttt_risk_event", "ttt_risk_refusal",
            "ttt_risk_kill"]
# __memcpy_chk and the like: with a bounds check (_FORTIFY_SOURCE, Ubuntu's default).
MEMORY = {"memcpy@plt", "__memcpy_chk@plt", "memmove@plt", "__memmove_chk@plt", "memcmp@plt", "__stack_chk_fail@plt",
          "memset@plt", "__memset_chk@plt"}

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


out = subprocess.run([sys.argv[1], "-d", "--no-show-raw-insn", sys.argv[2]],
                     check=True, capture_output=True, text=True).stdout
funcs, name = {}, None
for line in out.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
    if m:
        name = m[1]
        funcs[name] = []
        continue
    m = re.match(r"^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$", line)
    if m and name is not None:
        funcs[name].append((m[1], m[2]))


def calls(ins):
    direct = {re.search(r"<([^>]*)>", o)[1].split("+")[0] for mnem, o in ins
              if (mnem.startswith("call") or mnem.startswith("jmp")) and "<" in o}
    indirect = [o for mnem, o in ins if mnem.startswith("call") and "<" not in o]
    return direct, indirect


for subject in SUBJECTS:
    if subject not in funcs:
        check(False, f"{subject} found")
        continue
    seen, todo, bad = set(), [subject], []
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        direct, indirect = calls(funcs[f])
        bad += [f"{f}: indirect call {o}" for o in indirect]
        bad += [f"{f}: {m}" for m, _ in funcs[f] if m in ("syscall", "sysenter", "int")]
        for t in direct:
            if t.endswith("@plt"):
                if t not in MEMORY:
                    bad.append(f"{f}: calls {t}")
            elif t in funcs and t != f:
                todo.append(t)
    size = sum(len(funcs[f]) for f in seen)
    check(not bad, f"{'S6 (note 003)' if subject.startswith('ttt_risk') else 'S5 (note 002)'} {subject}: {len(seen)} functions, {size} instructions, calls only memory functions and the stack protector"
                   + (f": found {bad[:5]}" if bad else ""))

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
