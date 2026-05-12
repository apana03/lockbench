#!/usr/bin/env python3
"""Generate rank-change figures comparing lock ranking on Xeon vs Graviton2,
for every lock in each of the three benches.

Output PNGs go to results/deck/figures/.
"""

from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"
FIG_DIR = RESULTS / "deck" / "figures"

plt.rcParams.update({
    "figure.dpi": 130,
    "axes.grid": True,
    "grid.alpha": 0.20,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "legend.frameon": False,
    "font.size": 11,
})

BENCH_LABEL = {"wh": "wormhole (trie+hash+list)",
               "cds": "StripedMap (hash table)",
               "avl": "BronsonAVL (tree)"}

# Per-lock colour so the same lock keeps the same colour across all benches.
LOCK_COLOR = {
    # Wormhole-specific
    "default":  "#444444",
    "occ":      "#9467bd",
    "occ-opt":  "#8c564b",
    "pcpu-rw":  "#2ca02c",
    # Shared across all benches
    "tas":      "#d62728",
    "ttas":     "#ff7f0e",
    "cas":      "#bcbd22",
    "ticket":   "#17becf",
    "std":      "#1f77b4",
}


# ---------- Data ----------
def _to_float(x):
    if pd.isna(x) or x == "":
        return np.nan
    if isinstance(x, (int, float, np.floating, np.integer)):
        return float(x)
    s = str(x).replace(",", ".")
    try:
        return float(s)
    except ValueError:
        return np.nan


def load_all():
    frames = []
    for arch in ("x86_64", "aarch64"):
        for bench, suffix in (("wh",  "wh_compare/wh.csv"),
                              ("cds", "cdsbench/cdsbench.csv"),
                              ("avl", "avl_compare/cds_avl.csv")):
            df = pd.read_csv(RESULTS / arch / suffix, sep=";")
            for c in ("zipf_theta", "ns_op", "fairness_ratio"):
                if c in df.columns:
                    df[c] = df[c].apply(_to_float)
            df["arch"] = arch
            df["bench"] = bench
            df["lock_short"] = df["lock"].str.replace(r"^(wh|cds|avl)-", "", regex=True)
            frames.append(df)
    return pd.concat(frames, ignore_index=True)


def median_per_cell(raw):
    return (raw.groupby(["arch", "bench", "lock_short",
                          "key_range", "zipf_theta", "dist",
                          "read_pct", "threads"])
               ["ops_s"].median().reset_index())


def ranks_at_max_threads(agg, bench, key_range, zipf_theta, dist, read_pct):
    out = {}
    for arch in ("x86_64", "aarch64"):
        sub = agg[(agg.bench == bench) & (agg.arch == arch) &
                  (agg.key_range == key_range) & (agg.dist == dist) &
                  (agg.zipf_theta == zipf_theta) & (agg.read_pct == read_pct)]
        if sub.empty:
            continue
        maxT = sub.threads.max()
        ranked = (sub[sub.threads == maxT]
                    .sort_values("ops_s", ascending=False)
                    .reset_index(drop=True))
        out[arch] = {row.lock_short: (i + 1, row.ops_s / 1e6)
                     for i, row in ranked.iterrows()}
    return out


# ---------- Slopegraph (bump chart) ----------
def fig_rank_slopegraph(agg, bench, key_range, zipf_theta, dist, read_pct,
                         workload_label, fname, title_lines=None):
    ranks = ranks_at_max_threads(agg, bench, key_range, zipf_theta, dist, read_pct)
    if "x86_64" not in ranks or "aarch64" not in ranks:
        print(f"  skip {fname}: missing data")
        return

    xeon = ranks["x86_64"]
    grav = ranks["aarch64"]
    locks = sorted(set(xeon) & set(grav), key=lambda lk: xeon[lk][0])
    n = len(locks)

    fig, ax = plt.subplots(figsize=(11, max(5.5, 0.55 * n + 2.5)))

    # Plot lines between xeon rank (left, x=0) and graviton rank (right, x=1).
    for lk in locks:
        xr, xv = xeon[lk]
        gr, gv = grav[lk]
        color = LOCK_COLOR.get(lk, "#888888")
        # Bold if rank changes.
        lw = 3.5 if xr != gr else 1.8
        alpha = 1.0 if xr != gr else 0.45
        ax.plot([0, 1], [xr, gr], "-", color=color, lw=lw, alpha=alpha,
                marker="o", ms=11, markeredgecolor="white", markeredgewidth=1.5)
        # Label on the left
        ax.annotate(f"  {lk}  ({xv:.1f} M/s)",
                    xy=(0, xr), xytext=(-0.02, xr),
                    ha="right", va="center", fontsize=11,
                    color=color,
                    fontweight="bold" if xr != gr else "normal")
        # Label on the right
        delta = ""
        if xr != gr:
            arrow = "↑" if gr < xr else "↓"
            delta = f"  {arrow}{abs(xr - gr)}"
        ax.annotate(f"  {lk}  ({gv:.1f} M/s){delta}",
                    xy=(1, gr), xytext=(1.02, gr),
                    ha="left", va="center", fontsize=11,
                    color=color,
                    fontweight="bold" if xr != gr else "normal")

    ax.set_xlim(-0.45, 1.45)
    ax.set_ylim(n + 0.6, 0.4)  # rank 1 at top
    ax.set_yticks(range(1, n + 1))
    ax.set_yticklabels([f"#{i}" for i in range(1, n + 1)])
    ax.set_xticks([0, 1])
    ax.set_xticklabels(["Xeon E5-2650L v3\n(max T = 12)",
                         "Graviton2\n(max T = 8)"], fontsize=11)
    ax.tick_params(axis="x", length=0, pad=8)
    ax.tick_params(axis="y", length=0)
    for sp in ("top", "right", "left", "bottom"):
        ax.spines[sp].set_visible(False)
    ax.grid(False)
    # Horizontal guides at each rank
    for r in range(1, n + 1):
        ax.axhline(r, color="#dddddd", lw=0.6, zorder=-1)

    title = title_lines or [
        f"{BENCH_LABEL[bench]}: lock ranking changes between platforms",
        f"Workload: {workload_label}.  Solid bold lines = rank changed.",
    ]
    ax.set_title("\n".join(title), fontsize=12, pad=14)
    fig.tight_layout()
    fig.savefig(FIG_DIR / fname, bbox_inches="tight")
    plt.close(fig)


# ---------- Summary heatmap: per (lock × workload) rank change ----------
def fig_rank_change_heatmap(agg, fname):
    """For every bench × lock × workload, show rank delta from Xeon to Graviton."""
    workloads = [
        ("L1_cold",     1000,  None,  "uniform", 90),
        ("L1_warm",     1000,  0.99,  "zipfian", 90),
        ("L1_hot",      1000,  1.2,   "zipfian", 90),
        ("L1_extreme",  1000,  1.5,   "zipfian", 90),
        ("L3_cold",     100000, None, "uniform", 90),
        ("L3_warm",     100000, 0.99, "zipfian", 90),
        ("L3_hot",      100000, 1.2,  "zipfian", 90),
        ("L3_extreme",  100000, 1.5,  "zipfian", 90),
        ("L1_50r_u",    1000,  None,  "uniform", 50),
        ("L1_50r_z99",  1000,  0.99,  "zipfian", 50),
        ("L3_50r_u",    100000, None, "uniform", 50),
        ("L3_50r_z99",  100000, 0.99, "zipfian", 50),
    ]
    bench_locks = {
        "wh":  ["default", "tas", "ttas", "cas", "occ", "occ-opt", "pcpu-rw"],
        "cds": ["std", "tas", "ttas", "cas", "ticket"],
        "avl": ["std", "tas", "ttas", "cas", "ticket"],
    }
    # Build a matrix: rows = (bench, lock), cols = workload, value = rank delta
    rows = []
    labels = []
    for bench, locks in bench_locks.items():
        for lk in locks:
            row = []
            for wl_label, kr, zt, dist, rd in workloads:
                if zt is None:
                    sub = agg[(agg.bench == bench) & (agg.key_range == kr) &
                              (agg.dist == dist) & (agg.read_pct == rd)]
                else:
                    sub = agg[(agg.bench == bench) & (agg.key_range == kr) &
                              (agg.dist == dist) & (agg.zipf_theta == zt) &
                              (agg.read_pct == rd)]
                ranks = {}
                for arch in ("x86_64", "aarch64"):
                    sa = sub[sub.arch == arch]
                    if sa.empty:
                        ranks[arch] = None; continue
                    maxT = sa.threads.max()
                    ranked = (sa[sa.threads == maxT]
                                .sort_values("ops_s", ascending=False)
                                .reset_index(drop=True))
                    rk = {r.lock_short: i + 1 for i, r in ranked.iterrows()}
                    ranks[arch] = rk.get(lk)
                if ranks["x86_64"] is None or ranks["aarch64"] is None:
                    row.append(np.nan)
                else:
                    row.append(ranks["aarch64"] - ranks["x86_64"])
            rows.append(row)
            labels.append(f"{bench}: {lk}")
    mat = np.array(rows, dtype=float)

    fig, ax = plt.subplots(figsize=(14, 7))
    cmap = plt.cm.RdBu  # red = rose in rank (got better) on left, blue = fell
    # Center at 0; symmetric range
    vmax = np.nanmax(np.abs(mat))
    im = ax.imshow(mat, cmap=cmap.reversed(), vmin=-vmax, vmax=vmax, aspect="auto")
    ax.set_xticks(range(len(workloads)))
    ax.set_xticklabels([w[0] for w in workloads], rotation=40, ha="right")
    ax.set_yticks(range(len(labels)))
    ax.set_yticklabels(labels, fontsize=9)
    # Cell labels
    for i in range(mat.shape[0]):
        for j in range(mat.shape[1]):
            v = mat[i, j]
            if np.isnan(v):
                continue
            txt = "" if v == 0 else (f"+{int(v)}" if v > 0 else f"{int(v)}")
            ax.text(j, i, txt, ha="center", va="center", fontsize=9,
                    color="black" if abs(v) < vmax * 0.6 else "white")
    # Bench separators
    ax.axhline(6.5, color="black", lw=1)
    ax.axhline(11.5, color="black", lw=1)
    plt.colorbar(im, ax=ax,
                  label="Graviton rank − Xeon rank   (negative = lock moved UP on Graviton)")
    ax.set_title(
        "Rank change per (bench × lock × workload), max threads, 90% read / 5% insert / 5% remove (or 50/25/25)\n"
        "Cell value = (Graviton rank) − (Xeon rank).  Negative = lock improved its rank on Graviton.\n"
        "Numbers are signed deltas; blank cell = no rank change.",
        fontsize=11)
    fig.tight_layout()
    fig.savefig(FIG_DIR / fname, bbox_inches="tight")
    plt.close(fig)


# ---------- Main ----------
def main():
    print("Loading data...")
    raw = load_all()
    agg = median_per_cell(raw)
    print(f"  rows: {len(raw):,}  agg rows: {len(agg):,}")

    print("Generating slopegraph figures...")
    # Wormhole at L1_warm 90/5/5
    fig_rank_slopegraph(agg, "wh", 1000, 0.99, "zipfian", 90,
                         "L1-resident, zipf θ=0.99 (warm), 90% read / 5% insert / 5% remove",
                         "f10_rank_wh_L1warm.png")
    fig_rank_slopegraph(agg, "wh", 100000, 0.99, "zipfian", 90,
                         "L3-resident, zipf θ=0.99 (warm), 90% read / 5% insert / 5% remove",
                         "f11_rank_wh_L3warm.png")
    fig_rank_slopegraph(agg, "cds", 1000, 0.99, "zipfian", 90,
                         "L1-resident, zipf θ=0.99 (warm), 90% read / 5% insert / 5% remove",
                         "f12_rank_cds_L1warm.png")
    fig_rank_slopegraph(agg, "avl", 1000, 0.99, "zipfian", 90,
                         "L1-resident, zipf θ=0.99 (warm), 90% read / 5% insert / 5% remove",
                         "f13_rank_avl_L1warm.png")

    print("Generating cross-workload rank-change heatmap...")
    fig_rank_change_heatmap(agg, "f14_rank_change_heatmap.png")

    print("Done.")


if __name__ == "__main__":
    main()
