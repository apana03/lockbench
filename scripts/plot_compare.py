#!/usr/bin/env python3
"""Render a 2x2 throughput plot from cdsbench CSV output.

Usage: plot_compare.py <cds.csv> <out_dir>
Output: <out_dir>/cds_locks.png — one subplot per workload, one line per stripe lock.
"""
import sys
from pathlib import Path
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

cds_csv, out_dir = sys.argv[1], Path(sys.argv[2])
out_dir.mkdir(parents=True, exist_ok=True)

cds = pd.read_csv(cds_csv, sep=";", decimal=",")
cds["lock_base"] = cds["lock"].str.replace("cds-", "", regex=False)

def workload_label(row):
    rd, ins = int(row["read_pct"]), int(row["insert_pct"])
    if rd == 80 and ins == 10:
        return f"{row['dist']} 80/10/10"
    if rd == 90 and ins == 5:
        return "uniform 90/5/5 read-heavy"
    if rd == 20 and ins == 40:
        return "zipfian 20/40/40 write-heavy"
    return f"{row['dist']} {rd}/{ins}/{100-rd-ins}"

cds["workload"] = cds.apply(workload_label, axis=1)
workloads = sorted(cds["workload"].unique())

fig, axes = plt.subplots(2, 2, figsize=(13, 9))
for ax, wkl in zip(axes.flat, workloads):
    sub = cds[cds["workload"] == wkl]
    for lk, g in sub.groupby("lock_base"):
        g = g.sort_values("threads")
        ax.plot(g["threads"], g["ops_s"] / 1e6, marker="o", label=lk)
    ax.set_title(wkl)
    ax.set_xlabel("Threads")
    ax.set_ylabel("M ops/s")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
fig.suptitle("libcds StripedMap — stripe-lock comparison (cdsbench)", fontsize=12)
fig.tight_layout()
fig.savefig(out_dir / "cds_locks.png", dpi=120)
plt.close(fig)
print(f"Wrote {out_dir / 'cds_locks.png'}")
