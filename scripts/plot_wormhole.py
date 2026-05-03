#!/usr/bin/env python3
"""Render wormhole plots from per-arch CSVs.

Auto-discovers any results/<arch>/wh_compare/wh.csv. If multiple arches
are present, generates one set of per-arch plots plus a cross-arch
overlay (see plot_cross_arch.py for the dedicated cross-arch artifacts).

Outputs (per arch <A>):
  results/<A>/wh_compare/wh_locks.png         line plot, 4 workloads x 7 locks
  results/<A>/wh_compare/wh_threads_max.png   bar chart at max threads on that arch
  results/<A>/wh_compare/wh_occopt_focus.png  occ-opt vs rwlock comparison
"""
from pathlib import Path
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent

LOCKS = ["default", "rw", "tas", "ttas", "cas", "occ", "occ-opt"]
PALETTE = {
    "default": "#888",
    "rw":      "#444",
    "tas":     "tab:blue",
    "ttas":    "tab:green",
    "cas":     "tab:orange",
    "occ":     "tab:purple",
    "occ-opt": "tab:red",
}
STYLES = {lk: ("--" if lk == "default" else "-") for lk in LOCKS}


def workload(row):
    rd, ins = int(row["read_pct"]), int(row["insert_pct"])
    if rd == 80 and ins == 10:  return f"{row['dist']} 80/10/10"
    if rd == 90 and ins == 5:   return "uniform 90/5/5 read-heavy"
    if rd == 20 and ins == 40:  return "zipfian 20/40/40 write-heavy"
    return f"{row['dist']} {rd}/{ins}/{100-rd-ins}"


def load_one(csv_path):
    df = pd.read_csv(csv_path, sep=";", decimal=",")
    df["lk"] = df["lock"].str.replace("wh-", "", regex=False)
    df["workload"] = df.apply(workload, axis=1)
    return df


def plot_arch(arch, csv_path):
    out_dir = csv_path.parent
    df = load_one(csv_path)
    workloads = sorted(df.workload.unique())
    max_t = int(df.threads.max())

    # Plot 1: 2x2 line grid
    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    for ax, wkl in zip(axes.flat, workloads):
        sub = df[df.workload == wkl]
        for lk in LOCKS:
            g = sub[sub.lk == lk].sort_values("threads")
            if len(g) == 0:
                continue
            ax.plot(g.threads, g.ops_s / 1e6, marker="o",
                    color=PALETTE[lk], linestyle=STYLES[lk], linewidth=2, label=lk)
        ax.set_title(wkl, fontsize=11)
        ax.set_xlabel("Threads")
        ax.set_ylabel("M ops/s")
        ax.legend(fontsize=8, ncol=2)
        ax.grid(True, alpha=0.3)
        ax.set_xscale("log", base=2)
    fig.suptitle(f"Wormhole — pluggable lock comparison ({arch})", fontsize=13, y=0.995)
    fig.tight_layout()
    fig.savefig(out_dir / "wh_locks.png", dpi=130, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out_dir / 'wh_locks.png'}")

    # Plot 2: bar chart at max threads (whatever the arch's cap is)
    fig, ax = plt.subplots(figsize=(13, 5.5))
    x_idx = list(range(len(workloads)))
    width = 0.12
    for i, lk in enumerate(LOCKS):
        ys = []
        for wkl in workloads:
            g = df[(df.workload == wkl) & (df.lk == lk) & (df.threads == max_t)]
            ys.append(g.ops_s.mean() / 1e6 if len(g) else 0)
        offset = (i - 3) * width
        ax.bar([x + offset for x in x_idx], ys, width=width,
               color=PALETTE[lk], label=lk, edgecolor="black", linewidth=0.4)
    ax.set_xticks(x_idx)
    ax.set_xticklabels([w.replace(" ", "\n", 1) for w in workloads], fontsize=10)
    ax.set_ylabel(f"M ops/s @ {max_t} threads")
    ax.set_title(f"Wormhole — {max_t}-thread throughput by lock variant ({arch})", fontsize=12)
    ax.legend(fontsize=9, ncol=7, loc="upper right")
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_dir / "wh_threads_max.png", dpi=130, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out_dir / 'wh_threads_max.png'}")

    # Plot 3: occ-opt focus
    focus_locks = ["default", "rw", "cas", "occ", "occ-opt"]
    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    for ax, wkl in zip(axes.flat, workloads):
        sub = df[df.workload == wkl]
        for lk in focus_locks:
            g = sub[sub.lk == lk].sort_values("threads")
            if len(g) == 0:
                continue
            is_occopt = (lk == "occ-opt")
            ax.plot(g.threads, g.ops_s / 1e6,
                    marker="*" if is_occopt else "o",
                    markersize=14 if is_occopt else 7,
                    color=PALETTE[lk],
                    linestyle=STYLES[lk],
                    linewidth=3 if is_occopt else 1.5,
                    label=lk + (" ★" if is_occopt else ""))
        ax.set_title(wkl, fontsize=11)
        ax.set_xlabel("Threads")
        ax.set_ylabel("M ops/s")
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)
        ax.set_xscale("log", base=2)
    fig.suptitle(f"Wormhole — occ-opt (★) vs locked baselines ({arch})", fontsize=13, y=0.995)
    fig.tight_layout()
    fig.savefig(out_dir / "wh_occopt_focus.png", dpi=130, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out_dir / 'wh_occopt_focus.png'}")


def main():
    csvs = sorted(ROOT.glob("results/*/wh_compare/wh.csv"))
    if not csvs:
        # Backward compat: legacy single-arch path
        legacy = ROOT / "results" / "wh_compare" / "wh.csv"
        if legacy.exists():
            csvs = [legacy]
    if not csvs:
        print("No wh.csv found under results/. Run scripts/wh_compare.sh first.")
        return
    for csv in csvs:
        # arch is the directory name two levels up: results/<ARCH>/wh_compare/wh.csv
        arch = csv.parent.parent.name if csv.parent.parent != ROOT / "results" else "default"
        plot_arch(arch, csv)


if __name__ == "__main__":
    main()
