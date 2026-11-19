#!/usr/bin/env python3
"""
Draws the throughput vs. loss rate graph and prints a summary table.
 
Input is either a CSV (columns loss_pct, throughput_mbps, and optionally
record_size) or the sender's printed output saved in log files.
 
Usage:
  python3 tests/plot_results.py --csv results.csv -o throughput_vs_loss.png
  python3 tests/plot_results.py --logs 'logs/*.log'
"""
import argparse, csv, glob, re, statistics, sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker

SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]   # fixed categorical order
INK, INK_2, GRID = "#1f1f1e", "#5f5e58", "#e4e3de"


def load_csv(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append((r.get("record_size") or "all",
                         float(r["loss_pct"]), float(r["throughput_mbps"])))
    return rows


def load_logs(pattern):
    rows = []
    for path in sorted(glob.glob(pattern)):
        text = open(path).read()
        for block in text.split("=== BLAST Transfer Summary ===")[1:]:
            loss = re.search(r"Loss Rate Config\s*:\s*([\d.]+)", block)
            tput = re.search(r"Throughput\s*:\s*([\d.]+)", block)
            if loss and tput:
                rows.append(("all", float(loss.group(1)), float(tput.group(1))))
    return rows


def main():
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--csv")
    src.add_argument("--logs")
    ap.add_argument("-o", "--out", default="throughput_vs_loss.png")
    ap.add_argument("--title", default="BLAST throughput vs. packet loss rate")
    ap.add_argument("--subtitle", default="")
    ap.add_argument("--logy", choices=["auto", "on", "off"], default="auto",
                    help="log y-axis; auto = on when max/min mean > 30")
    args = ap.parse_args()

    rows = load_csv(args.csv) if args.csv else load_logs(args.logs)
    if not rows:
        sys.exit("no data found")

    groups = defaultdict(lambda: defaultdict(list))      # series -> loss -> [tput]
    for series, loss, tput in rows:
        groups[series][loss].append(tput)
    names = sorted(groups, key=lambda s: (s == "all", int(s) if s.isdigit() else 0))
    if len(names) > len(SERIES):
        sys.exit("more than 4 series: facet the data instead")

    plt.rcParams.update({"font.size": 11, "axes.edgecolor": GRID, "axes.labelcolor": INK_2,
                         "xtick.color": INK_2, "ytick.color": INK_2})
    fig, ax = plt.subplots(figsize=(8, 4.8), dpi=200)

    print("| Series | Loss % | Mean MB/s | Std dev | Min | Max | Runs |")
    print("|---|---|---|---|---|---|---|")
    for i, name in enumerate(names):
        losses = sorted(groups[name])
        vals = [groups[name][l] for l in losses]
        mean = [statistics.mean(v) for v in vals]
        lo = [m - min(v) for m, v in zip(mean, vals)]
        hi = [max(v) - m for m, v in zip(mean, vals)]
        label = "all runs" if name == "all" else f"{name}-byte records"
        ax.errorbar(losses, mean, yerr=[lo, hi], color=SERIES[i], lw=2, marker="o", ms=8,
                    mec="white", mew=2, capsize=0, elinewidth=1.5, label=label, zorder=3)
        # direct label on the last point only
        ax.annotate(f"{mean[-1]:.2f} MB/s", (losses[-1], mean[-1]), xytext=(8, 0),
                    textcoords="offset points", va="center", color=INK, fontsize=10)
        for l, v, m in zip(losses, vals, mean):
            sd = statistics.stdev(v) if len(v) > 1 else 0.0
            print(f"| {label} | {l:g} | {m:.3f} | {sd:.3f} | {min(v):.3f} | {max(v):.3f} | {len(v)} |")

    ax.set_xlabel("Packet loss rate injected by garbler (%)")
    ax.set_ylabel("Throughput (MB/s)")
    means = [statistics.mean(v) for g in groups.values() for v in g.values()]
    use_log = args.logy == "on" or (args.logy == "auto" and min(means) > 0
                                     and max(means) / min(means) > 30)
    if use_log:
        ax.set_yscale("log")
        ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
        ax.set_ylabel("Throughput (MB/s, log scale)")
    else:
        ax.set_ylim(bottom=0)
    ax.set_xlim(left=-1)
    all_losses = sorted({l for g in groups.values() for l in g})
    ax.set_xticks(all_losses)
    ax.grid(axis="y", which="major", color=GRID, lw=1)
    ax.spines[["top", "right"]].set_visible(False)
    fig.suptitle(args.title, x=0.06, ha="left", fontsize=13, color=INK, fontweight="bold")
    sub = args.subtitle or "Mean of all runs per loss rate; whiskers show min-max"
    ax.set_title(sub, loc="left", fontsize=10, color=INK_2)
    if len(names) > 1:
        ax.legend(frameon=False, loc="upper right")
    fig.tight_layout()
    fig.savefig(args.out)
    print(f"\nsaved {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
