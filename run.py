#!/usr/bin/env python3
"""Results report: RESULTS.md and the README's figures, generated from committed JSON.

  python run.py report          write RESULTS.md and fill the README's result blocks
  python run.py report --check  regenerate in memory; exit 1 naming any difference

As the three repos' reports (lockfree-ipc's note 007), and built on
lockfree-ipc's: its environment checker, conditions table and formatting are
used from the copy CMake fetched at the pinned commit
(build/_deps/lockfree_ipc-src), so a run here is held to the same rules. Only
files listed in results/published.toml are read, each validated first:
environment checker, published, bench mode, clean build, its component's block
present, one commit, machine and kernel command line per set. Figures are
min-max over a set's runs. Standard library only.
"""
import argparse
import importlib.util
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

SETS = ("sink", "risk")
BLOCK = re.compile(r"(<!-- results:(?P<name>[a-z0-9-]+) -->\n)(?P<body>.*?)(<!-- /results -->)", re.S)
MEAN = re.compile(r"\b(mean|average|avg)\b", re.I)


class ReportError(Exception):
    pass


def harness(root, lfipc=None):
    """lockfree-ipc's run.py, from the fetched copy, as a module."""
    path = (Path(lfipc) if lfipc else root / "build" / "_deps" / "lockfree_ipc-src") / "run.py"
    if not path.is_file():
        raise ReportError(f"{path} missing: configure the build first (cmake -S . -B build)")
    spec = importlib.util.spec_from_file_location("lfipc_run", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod, path.parent


def validate(root, lf_root, name, files):
    runs = []
    for f in files:
        path = root / "results" / f
        if not path.is_file():
            raise ReportError(f"{name}: {f}: missing")
        check = subprocess.run([sys.executable, "-I", str(lf_root / "tests" / "check_environment_json.py"), str(path)],
                               capture_output=True, text=True)
        if check.returncode != 0:
            fails = [l for l in check.stdout.splitlines() if l.startswith("FAIL")]
            raise ReportError(f"{name}: {f}: environment check failed: {fails[:2]}")
        d = json.loads(path.read_text())
        e = d["environment"]
        if not (e["published"] and e["ok"] and e["bench_mode"]["ok"]):
            raise ReportError(f"{name}: {f}: not a published run (refused or outside results/)")
        if not e["build"]["clean"]:
            raise ReportError(f"{name}: {f}: built from a tree with uncommitted changes")
        if name not in d or not d.get("clock", {}).get("ok"):
            raise ReportError(f"{name}: {f}: no {name} block, or the clock refused")
        runs.append((f, d))
    if not runs:
        raise ReportError(f"{name}: no runs listed")
    for key, get in (("commit", lambda e: e["build"]["commit"]),
                     ("machine", lambda e: (e["machine"]["product"], e["cpu"]["model"], e["cpu"]["microcode"])),
                     ("kernel command line", lambda e: e["kernel"]["cmdline"])):
        if len({get(d["environment"]) for _, d in runs}) > 1:
            raise ReportError(f"{name}: runs mix more than one {key}")
    return runs


def load_sets(root, lf_root):
    manifest = root / "results" / "published.toml"
    if not manifest.is_file():
        return {}
    m = tomllib.loads(manifest.read_text())
    unknown = set(m) - set(SETS)
    if unknown:
        raise ReportError(f"published.toml: unknown sets {sorted(unknown)}")
    return {n: (m[n].get("note", ""), validate(root, lf_root, n, m[n]["runs"])) for n in SETS if n in m}


# ---------------------------------------------------------------- note 002


def sink_rows(lf, runs):
    ss = [d["sink"] for _, d in runs]
    names = [c["name"] for c in ss[0]["cases"]]
    for s in ss:
        if [c["name"] for c in s["cases"]] != names or any(c["set_aside"] for c in s["cases"]):
            raise ReportError("sink: runs took different cases, or an event found the ring full")
    rows = []
    for i, n in enumerate(names):
        cs = [s["cases"][i] for s in ss]
        rows.append([n, lf.rng([c["messages_per_s"] / 1e6 for c in cs], lambda v: f"{v:.2f}")]
                    + [lf.rng([c["cost"][f] for c in cs]) for f in ("p50_ns", "p99_ns", "p999_ns", "max_ns")]
                    + [f"{cs[0]['events']:,}" if cs[0]["instruments"] else "–"])
    return rows


SINK_HEAD = ["Case", "M messages/s", "p50", "p99", "p99.9", "max", "Book events"]


def sink_section(lf, note, runs):
    s0 = runs[0][1]["sink"]
    return [f"## The strategy's sink (note {note})", "",
            f"The first {s0['messages']:,} messages of the sample day `{s0['source']}` (the open, 09:29:30 for five minutes, among "
            "them), decoded into memory first, through a fresh default book and the feed handler's handler on one pinned CPU "
            f"(CPU {s0['cpu']}): with a sink that does nothing, with the feed handler's publisher and its state table (no consumer), "
            "and with the strategy's sink trading the lists of 1, 10 and 100 instruments with the most best-bid changes over the open "
            f"(note 002, D5), each book event pushed into the ring to the strategy's thread on CPU {s0['strategy_cpu']}, which pops "
            "it. Each message's handling timed between start() and stop(), unsubtracted, the first "
            f"{s0['warmup_discarded']:,} discarded. No event found the ring full. {len(runs)} runs; ranges over them.", "",
            lf.table(SINK_HEAD, sink_rows(lf, runs)), ""]


# ---------------------------------------------------------------- note 003


def risk_rows(lf, runs):
    rs = [d["risk"] for _, d in runs]
    names = [c["name"] for c in rs[0]["cases"]]
    for r in rs:
        if [c["name"] for c in r["cases"]] != names or not r["same_as_reference"]:
            raise ReportError("risk: runs took different cases, or the checks and the reference sent different intents")
    rows = []
    for i, n in enumerate(names):
        cs = [r["cases"][i] for r in rs]
        rows.append([n, lf.rng([c["events_per_s"] / 1e6 for c in cs], lambda v: f"{v:.2f}")]
                    + [lf.rng([c["event"][f] for c in cs]) for f in ("p50_ns", "p99_ns", "p999_ns")]
                    + ([lf.rng([c["check"][f] for c in cs]) for f in ("p50_ns", "p99_ns")] if cs[0]["check"]["count"] else ["–", "–"])
                    + [lf.rng([c["answer"][f] for c in cs]) for f in ("p50_ns", "p99_ns")]
                    + [f"{cs[0]['intents']:,}", f"{cs[0]['refused']:,}"])
    return rows


RISK_HEAD = ["Case", "M events/s", "Book event p50", "p99", "p99.9", "Check p50", "p99", "Answer p50", "p99", "Intents", "Refused"]


def risk_section(lf, note, runs):
    r0 = runs[0][1]["risk"]
    return [f"## Pre-trade risk (note {note})", "",
            f"The strategy's thread on one pinned CPU (CPU {r0['cpu']}), given the {r0['book_events']:,} book events the strategy's "
            f"sink gives for D5's {r0['instruments']} instruments over the first {r0['messages']:,} messages of the sample day "
            f"`{r0['source']}` (collected first, into memory), the strategy's intents answered by note 002's model venue: with no "
            "checks, through the risk checks (the default limits, `config/limits.conf`), and through the reference implementation. "
            "Timed between start() and stop(), unsubtracted, each apart: a book event's handling (the checks', the strategy's, and "
            "the check of every intent it sends), an intent's check alone, an answer's handling (the checks' and the strategy's). "
            f"The venue's own work is not timed. The first {r0['warmup_discarded']:,} book events discarded. The checks and the "
            f"reference sent the same intents in every run. {len(runs)} runs; ranges over them.", "",
            lf.table(RISK_HEAD, risk_rows(lf, runs)), ""]


def results_md(lf, sets):
    lines = ["# Results", "",
             "Generated by `python run.py report` from the runs listed in `results/published.toml`. "
             "Do not edit: `python run.py report --check` fails if this file and the JSON disagree. "
             "Latencies in ns unless marked µs; percentiles only.", ""]
    if not sets:
        return "\n".join(lines + ["No published results yet.", ""]).rstrip() + "\n"
    lines += lf.conditions(sets)
    if "sink" in sets:
        lines += sink_section(lf, *sets["sink"])
    if "risk" in sets:
        lines += risk_section(lf, *sets["risk"])
    return "\n".join(lines).rstrip() + "\n"


def readme_blocks(lf, sets):
    blocks = {"sink-summary": "The strategy's sink: not yet measured (design note 002).\n",
              "risk-summary": "Pre-trade risk: not yet measured (design note 003).\n"}
    if "risk" in sets:
        runs = sets["risk"][1]
        blocks["risk-summary"] = (lf.table(RISK_HEAD, risk_rows(lf, runs))
                                  + "\n\nThe strategy's thread on one CPU over the sample day's first "
                                  f"{runs[0][1]['risk']['messages']:,} messages' book events for 100 instruments, with no checks, the risk "
                                  "checks, or their reference implementation; ns, range over runs. The method: [RESULTS.md](RESULTS.md).\n")
    if "sink" in sets:
        runs = sets["sink"][1]
        blocks["sink-summary"] = (lf.table(SINK_HEAD, sink_rows(lf, runs))
                                  + f"\n\nThe sample day's first {runs[0][1]['sink']['messages']:,} messages through the feed handler's "
                                  "handler on one CPU, with no sink, its publisher, or the strategy's sink; ns a message, range over "
                                  "runs. The method: [RESULTS.md](RESULTS.md).\n")
    return blocks


def report(root, check, lfipc=None):
    lf, lf_root = harness(root, lfipc)
    sets = load_sets(root, lf_root)
    results = results_md(lf, sets)
    readme_path = root / "README.md"
    readme = readme_path.read_text()
    blocks = readme_blocks(lf, sets)
    found = {m["name"] for m in BLOCK.finditer(readme)}
    if set(blocks) - found:
        raise ReportError(f"README.md has no block for {sorted(set(blocks) - found)}")
    new_readme = BLOCK.sub(lambda m: m.group(1) + blocks.get(m["name"], m["body"]) + m.group(4), readme)
    for text, what in ((results, "RESULTS.md"), (new_readme, "README.md")):
        if MEAN.findall(text):
            raise ReportError(f"{what}: generated text mentions {MEAN.findall(text)}")
    if not check:
        (root / "RESULTS.md").write_text(results)
        readme_path.write_text(new_readme)
        print(f"wrote RESULTS.md and {len(blocks)} README block(s)")
        return 0
    diffs = []
    current = (root / "RESULTS.md").read_text() if (root / "RESULTS.md").is_file() else ""
    if current != results:
        diffs.append("RESULTS.md")
    old = {m["name"]: m["body"] for m in BLOCK.finditer(readme)}
    new = {m["name"]: m["body"] for m in BLOCK.finditer(new_readme)}
    diffs += [f"README.md block '{k}'" for k in new if old.get(k) != new[k]]
    if diffs:
        print("report --check: out of date: " + ", ".join(diffs) + " (run: python run.py report)")
        return 1
    print("report --check: RESULTS.md and README.md agree with the published JSON")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("report", help="generate RESULTS.md and README blocks from published JSON")
    rp.add_argument("--check", action="store_true", help="compare only; exit 1 on any difference")
    rp.add_argument("--root", type=Path, default=Path(__file__).resolve().parent, help=argparse.SUPPRESS)
    rp.add_argument("--lockfree-ipc", dest="lfipc", help="the fetched lockfree-ipc (default: build/_deps/lockfree_ipc-src)")
    args = ap.parse_args()
    try:
        return report(args.root, args.check, args.lfipc)
    except ReportError as e:
        print(f"report: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
