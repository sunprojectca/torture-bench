#!/usr/bin/env python3
"""
compare.py — compare two torture-bench JSON result files.

Usage:
    python3 compare.py machine_a.json machine_b.json
    python3 compare.py machine_a.json machine_b.json --csv scores.csv
"""

import json
import sys
import os

ANSI_RESET  = "\033[0m"
ANSI_GREEN  = "\033[92m"
ANSI_YELLOW = "\033[93m"
ANSI_RED    = "\033[91m"
ANSI_BOLD   = "\033[1m"
ANSI_CYAN   = "\033[96m"

def load(path):
    with open(path) as f:
        return json.load(f)

def shorten_cpu(s):
    replacements = [
        "Intel(R) Core(TM) ", "", "AMD ", "", "Apple ", "",
        "  ", " ", "(R)", "", "(TM)", "",
    ]
    for old, new in zip(replacements[::2], replacements[1::2]):
        s = s.replace(old, new)
    return s.strip()[:40]

def color_ratio(ratio):
    if ratio >= 1.2:   return ANSI_GREEN
    if ratio >= 0.85:  return ANSI_YELLOW
    return ANSI_RED

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    csv_out = None
    if "--csv" in sys.argv:
        idx = sys.argv.index("--csv")
        if idx + 1 < len(sys.argv):
            csv_out = sys.argv[idx + 1]

    if len(args) < 2:
        print("Usage: compare.py <a.json> <b.json> [--csv out.csv]")
        sys.exit(1)

    a = load(args[0])
    b = load(args[1])

    cpu_a = shorten_cpu(a["platform"]["cpu"])
    cpu_b = shorten_cpu(b["platform"]["cpu"])
    arch_a = a["platform"]["arch"]
    arch_b = b["platform"]["arch"]

    print(f"\n{ANSI_BOLD}  torture-bench — Machine Comparison{ANSI_RESET}")
    print("  " + "─"*70)
    print(f"  {'Machine A':<30}  {'Machine B':<30}")
    print(f"  {cpu_a:<30}  {cpu_b:<30}")
    print(f"  {arch_a} / {a['platform']['logical_cores']} cores{'':<15}"
          f"  {arch_b} / {b['platform']['logical_cores']} cores")

    warn_a = a.get("coprocessor_warnings", 0)
    warn_b = b.get("coprocessor_warnings", 0)
    print(f"  Verdict: {a.get('verdict','?'):<28}  {b.get('verdict','?')}")
    if warn_a or warn_b:
        print(f"  {ANSI_YELLOW}⚠  Coprocessor warnings: A={warn_a}  B={warn_b}{ANSI_RESET}")
    print("  " + "─"*70)

    mods_a = {m["name"]: m for m in a.get("modules", [])}
    mods_b = {m["name"]: m for m in b.get("modules", [])}
    all_names = list(dict.fromkeys(list(mods_a.keys()) + list(mods_b.keys())))

    header = f"  {'Module':<22}  {'Score A':>10}  {'Score B':>10}  {'B/A':>7}  {'Winner'}"
    print(header)
    print("  " + "─"*70)

    csv_rows = [["module", "score_a", "score_b", "ratio", "winner",
                 "warn_a", "warn_b"]]

    wins_a = wins_b = ties = 0
    for name in all_names:
        ma = mods_a.get(name)
        mb = mods_b.get(name)
        if not ma or not mb:
            print(f"  {name:<22}  {'N/A':>10}  {'N/A':>10}  {'—':>7}")
            continue

        sa = ma["score"]
        sb = mb["score"]
        ratio = sb / sa if sa > 0 else 0.0
        warn_ma = "!" if ma.get("coprocessor_suspected") else " "
        warn_mb = "!" if mb.get("coprocessor_suspected") else " "

        if ratio >= 1.05:
            winner = f"{ANSI_GREEN}B{ANSI_RESET}"
            wins_b += 1
        elif ratio <= 0.95:
            winner = f"{ANSI_RED}A{ANSI_RESET}"
            wins_a += 1
        else:
            winner = f"{ANSI_YELLOW}TIE{ANSI_RESET}"
            ties += 1

        col = color_ratio(ratio)
        print(f"  {name:<22}  {sa:>10.3f}{warn_ma} {sb:>10.3f}{warn_mb} "
              f" {col}{ratio:>6.2f}x{ANSI_RESET}  {winner}")

        csv_rows.append([name, f"{sa:.4f}", f"{sb:.4f}", f"{ratio:.4f}",
                         "B" if ratio>=1.05 else "A" if ratio<=0.95 else "TIE",
                         warn_ma.strip(), warn_mb.strip()])

    print("  " + "─"*70)
    comp_a = a.get("composite_score", 0.0)
    comp_b = b.get("composite_score", 0.0)
    comp_ratio = comp_b / comp_a if comp_a > 0 else 0.0
    col = color_ratio(comp_ratio)
    print(f"  {'COMPOSITE':<22}  {comp_a:>10.3f}   {comp_b:>10.3f}  "
          f"{col}{comp_ratio:>6.2f}x{ANSI_RESET}")
    print(f"\n  Wins → A:{wins_a}  B:{wins_b}  Ties:{ties}")
    print(f"  Chain hashes: A={a.get('chain_proof_hash','?')}  "
          f"B={b.get('chain_proof_hash','?')}")
    print()

    if warn_a or warn_b:
        print(f"  {ANSI_YELLOW}NOTE: Modules marked '!' detected coprocessor acceleration.")
        print(f"  Compare only PURE_CPU modules for a fair CPU-only score.{ANSI_RESET}\n")

    if csv_out:
        import csv
        with open(csv_out, "w", newline="") as cf:
            w = csv.writer(cf)
            w.writerows(csv_rows)
        print(f"  CSV written to: {csv_out}\n")

if __name__ == "__main__":
    main()
