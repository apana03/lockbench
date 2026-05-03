#!/usr/bin/env python3
"""Cross-architecture plots: throughput overlay + fairness comparison.

Globs all results/<arch>/<bench>/<csv> and produces:
  results/cross_arch/throughput_<bench>_<workload>.png
      Per-workload overlay: each lock primitive as one line; one line color
      per lock, line style per architecture. Common thread points only.

  results/cross_arch/fairness_<bench>_<workload>.png
      Bar chart: x-axis = lock; one cluster per arch; bar height =
      fairness_ratio at max threads on that arch.

Run after each machine has populated results/<arch>/.
"""
from pathlib import Path
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "results" / "cross_arch"
OUT_DIR.mkdir(parents=True, exist_ok=True)


def workload(row):
    rd, ins = int(row["read_pct"]), int(row["insert_pct"])
    if rd == 80 and ins == 10:  return f"{row['dist']}_80_10_10"
    if rd == 90 and ins == 5:   return "uniform_90_5_5_read_heavy"
    if rd == 20 and ins == 40:  return "zipfian_20_40_40_write_heavy"
    return f"{row['dist']}_{rd}_{ins}_{100-rd-ins}"


def load_bench(bench, csv_glob):
    """Load all matching CSVs, return concatenated DataFrame with arch + lk + workload."""
    csvs = sorted(ROOT.glob(csv_glob))
    if not csvs:
        return None
    frames = []
    for csv in csvs:
        df = pd.read_csv(csv, sep=";", decimal=",")
        # arch column should be first; if missing (legacy CSV), infer from path.
        if "arch" not in df.columns:
            arch_name = csv.parent.parent.name
            df.insert(0, "arch", arch_name)
        # lk = lock with bench-specific prefix stripped
        prefix = {"wh": "wh-", "cds": "cds-", "avl": "avl-"}[bench]
        df["lk"] = df["lock"].str.replace(prefix, "", regex=False)
        df["workload"] = df.apply(workload, axis=1)
        frames.append(df)
    return pd.concat(frames, ignore_index=True)


# Color palettes per bench (lock universe differs)
WH_LOCKS = ["default", "rw", "tas", "ttas", "cas", "occ", "occ-opt"]
CDS_LOCKS = ["std", "tas", "ttas", "cas", "ticket"]
AVL_LOCKS = CDS_LOCKS

PALETTES = {
    "wh": {"default":"#888","rw":"#444","tas":"tab:blue","ttas":"tab:green",
           "cas":"tab:orange","occ":"tab:purple","occ-opt":"tab:red"},
    "cds": {"std":"#888","tas":"tab:blue","ttas":"tab:green","cas":"tab:orange","ticket":"tab:red"},
    "avl": {"std":"#888","tas":"tab:blue","ttas":"tab:green","cas":"tab:orange","ticket":"tab:red"},
}
LOCK_SETS = {"wh": WH_LOCKS, "cds": CDS_LOCKS, "avl": AVL_LOCKS}

# Architecture line styles for throughput overlays.
ARCH_STYLES = {0: "-", 1: "--", 2: ":", 3: "-."}
ARCH_MARKERS = {0: "o", 1: "s", 2: "^", 3: "D"}


def plot_throughput(bench, df, label):
    """One PNG per workload, all locks × all archs."""
    arches = sorted(df.arch.unique())
    workloads = sorted(df.workload.unique())
    locks = LOCK_SETS[bench]
    palette = PALETTES[bench]
    arch_idx = {a: i for i, a in enumerate(arches)}

    for wkl in workloads:
        sub = df[df.workload == wkl]
        if len(sub) == 0:
            continue
        fig, ax = plt.subplots(figsize=(13, 7))
        for arch in arches:
            ai = arch_idx[arch]
            for lk in locks:
                g = sub[(sub.arch == arch) & (sub.lk == lk)].sort_values("threads")
                if len(g) == 0:
                    continue
                ax.plot(g.threads, g.ops_s / 1e6,
                        marker=ARCH_MARKERS[ai % 4],
                        linestyle=ARCH_STYLES[ai % 4],
                        color=palette[lk],
                        linewidth=2,
                        label=f"{lk} ({arch})")
        ax.set_xlabel("Threads (log scale)")
        ax.set_ylabel("M ops/s")
        ax.set_xscale("log", base=2)
        ax.set_title(f"{label} — throughput across architectures — {wkl}", fontsize=12)
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=8, ncol=2, loc="best")
        fig.tight_layout()
        out = OUT_DIR / f"throughput_{bench}_{wkl}.png"
        fig.savefig(out, dpi=130, bbox_inches="tight")
        plt.close(fig)
        print(f"Wrote {out}")


def plot_fairness(bench, df, label):
    """Bar chart of fairness_ratio at each arch's max thread count."""
    arches = sorted(df.arch.unique())
    workloads = sorted(df.workload.unique())
    locks = LOCK_SETS[bench]
    palette = PALETTES[bench]

    for wkl in workloads:
        sub = df[df.workload == wkl]
        if len(sub) == 0:
            continue
        fig, ax = plt.subplots(figsize=(13, 5.5))
        x_idx = list(range(len(locks)))
        n_arch = len(arches)
        width = 0.8 / max(1, n_arch)
        for ai, arch in enumerate(arches):
            arch_sub = sub[sub.arch == arch]
            if len(arch_sub) == 0:
                continue
            max_t = int(arch_sub.threads.max())
            ys = []
            for lk in locks:
                g = arch_sub[(arch_sub.lk == lk) & (arch_sub.threads == max_t)]
                ys.append(float(g.fairness_ratio.mean()) if len(g) else 0.0)
            offset = (ai - (n_arch - 1) / 2) * width
            bars = ax.bar([x + offset for x in x_idx], ys, width=width,
                          label=f"{arch} ({max_t}T)", edgecolor="black", linewidth=0.4)
            # Color each bar by lock for visual continuity.
            for bar, lk in zip(bars, locks):
                bar.set_color(palette[lk])
                bar.set_alpha(0.5 + 0.5 * (n_arch - 1 - ai) / max(1, n_arch - 1))
        ax.set_xticks(x_idx)
        ax.set_xticklabels(locks, fontsize=10)
        ax.set_ylim(0, 1.05)
        ax.axhline(1.0, color="green", linewidth=0.5, linestyle="--", alpha=0.5)
        ax.set_ylabel("Fairness ratio (min/max per-thread ops; 1.0 = perfect)")
        ax.set_title(f"{label} — fairness at max-threads-per-arch — {wkl}", fontsize=12)
        ax.legend(fontsize=9)
        ax.grid(True, axis="y", alpha=0.3)
        fig.tight_layout()
        out = OUT_DIR / f"fairness_{bench}_{wkl}.png"
        fig.savefig(out, dpi=130, bbox_inches="tight")
        plt.close(fig)
        print(f"Wrote {out}")


def main():
    benches = [
        ("wh", "results/*/wh_compare/wh.csv", "Wormhole"),
        ("cds", "results/*/avl_compare/cds_striped.csv", "libcds StripedMap"),
        ("avl", "results/*/avl_compare/cds_avl.csv", "libcds BronsonAVL"),
    ]
    for bench, glob, label in benches:
        df = load_bench(bench, glob)
        if df is None or df.empty:
            print(f"Skipping {bench}: no CSV matched {glob}")
            continue
        n_arch = df.arch.nunique()
        print(f"\n=== {label} ({n_arch} arch{'es' if n_arch != 1 else ''}: {sorted(df.arch.unique())}) ===")
        plot_throughput(bench, df, label)
        plot_fairness(bench, df, label)


if __name__ == "__main__":
    main()
