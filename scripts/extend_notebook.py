#!/usr/bin/env python3
"""Append wormhole sections to scripts/avl_compare.ipynb."""
import json, uuid
from pathlib import Path

NB = Path("scripts/avl_compare.ipynb")
nb = json.load(open(NB))

def md(text):
    return {"cell_type": "markdown", "id": uuid.uuid4().hex[:8],
            "metadata": {}, "source": text.splitlines(keepends=True)}

def code(text):
    return {"cell_type": "code", "id": uuid.uuid4().hex[:8],
            "metadata": {}, "execution_count": None, "outputs": [],
            "source": text.splitlines(keepends=True)}

# Update intro to mention wormhole
nb["cells"][0]["source"] = [
    "# libcds StripedMap vs BronsonAVLTreeMap vs Wormhole — lock primitive comparison\n",
    "\n",
    "Three concurrent indexes, each with a pluggable lock primitive at a different\n",
    "structural level (StripedMap: per-stripe; BronsonAVL: per-node monitor;\n",
    "Wormhole: per-leaf rwlock + per-leaf spinlock).\n",
    "\n",
    "**StripedMap & BronsonAVL** use these primitives:\n",
    "\n",
    "| Lock     | Description |\n",
    "|----------|-------------|\n",
    "| `std`    | `std::mutex` baseline |\n",
    "| `tas`    | atomic_flag test-and-set spinlock |\n",
    "| `ttas`   | test-and-test-and-set spinlock |\n",
    "| `cas`    | CAS-based exchange spinlock |\n",
    "| `ticket` | strict-FIFO ticket lock |\n",
    "\n",
    "**Wormhole** uses a different set (ticket excluded — see EXPERIMENT.md):\n",
    "\n",
    "| Lock      | Description |\n",
    "|-----------|-------------|\n",
    "| `default` | Wu's stock rwlock (upstream wormhole, no shim) |\n",
    "| `rw`      | `rw_lock` — true reader-writer lock |\n",
    "| `tas`     | exclusive-only via shim |\n",
    "| `ttas`    | exclusive-only via shim |\n",
    "| `cas`     | exclusive-only via shim |\n",
    "| `occ`     | exclusive-only via shim (seqlock-as-mutex) |\n",
]

# Update load cell to also pull wormhole
nb["cells"][1]["source"] = [
    "import pandas as pd\n",
    "import matplotlib.pyplot as plt\n",
    "from pathlib import Path\n",
    "\n",
    "RESULTS_AVL = Path('../results/avl_compare')\n",
    "RESULTS_WH  = Path('../results/wh_compare')\n",
    "cds = pd.read_csv(RESULTS_AVL / 'cds_striped.csv', sep=';', decimal=',')\n",
    "avl = pd.read_csv(RESULTS_AVL / 'cds_avl.csv',     sep=';', decimal=',')\n",
    "wh  = pd.read_csv(RESULTS_WH  / 'wh.csv',          sep=';', decimal=',')\n",
    "\n",
    "cds['lk']  = cds['lock'].str.replace('cds-', '', regex=False)\n",
    "avl['lk']  = avl['lock'].str.replace('avl-', '', regex=False)\n",
    "wh['lk']   = wh['lock'].str.replace('wh-',   '', regex=False)\n",
    "cds['ds']  = 'StripedMap'\n",
    "avl['ds']  = 'BronsonAVL'\n",
    "wh['ds']   = 'Wormhole'\n",
    "\n",
    "def workload(row):\n",
    "    rd, ins = int(row['read_pct']), int(row['insert_pct'])\n",
    "    if rd == 80 and ins == 10:  return f\"{row['dist']} 80/10/10\"\n",
    "    if rd == 90 and ins == 5:   return 'uniform 90/5/5 read-heavy'\n",
    "    if rd == 20 and ins == 40:  return 'zipfian 20/40/40 write-heavy'\n",
    "    return f\"{row['dist']} {rd}/{ins}/{100-rd-ins}\"\n",
    "for df in (cds, avl, wh):\n",
    "    df['workload'] = df.apply(workload, axis=1)\n",
    "\n",
    "for name, df in [('StripedMap', cds), ('BronsonAVL', avl), ('Wormhole', wh)]:\n",
    "    print(f'{name} rows: {len(df)} variants: {sorted(df.lk.unique())}')\n",
]

# Add wormhole-specific cells before the cross-DS comparison
new_cells = [
    md("## Plot 3 — Wormhole, per-leaf-lock comparison\n"
       "\n"
       "`default` (Wu's stock rwlock) is dashed grey, `rw` (true rwlock) is\n"
       "solid grey, the rest are exclusive-only. The interesting question is:\n"
       "how much does losing reader concurrency cost on a structure designed\n"
       "around it?\n"),
    code(
        "def plot_wh(df, title, figsize=(13,9)):\n"
        "    workloads = sorted(df.workload.unique())\n"
        "    fig, axes = plt.subplots(2, 2, figsize=figsize)\n"
        "    palette = {'default':'#888','rw':'#444','tas':'tab:blue',\n"
        "               'ttas':'tab:green','cas':'tab:orange','occ':'tab:purple'}\n"
        "    styles  = {'default':'--','rw':'-','tas':'-','ttas':'-','cas':'-','occ':'-'}\n"
        "    for ax, wkl in zip(axes.flat, workloads):\n"
        "        sub = df[df.workload == wkl]\n"
        "        for lk in ['default','rw','tas','ttas','cas','occ']:\n"
        "            g = sub[sub.lk == lk].sort_values('threads')\n"
        "            if len(g) == 0: continue\n"
        "            ax.plot(g.threads, g.ops_s/1e6, marker='o',\n"
        "                    color=palette[lk], linestyle=styles[lk], label=lk)\n"
        "        ax.set_title(wkl); ax.set_xlabel('Threads'); ax.set_ylabel('M ops/s')\n"
        "        ax.legend(fontsize=8); ax.grid(True, alpha=0.3)\n"
        "    fig.suptitle(title)\n"
        "    fig.tight_layout()\n"
        "    return fig\n"
        "\n"
        "plot_wh(wh, 'Wormhole — pluggable per-leaf lock');\n"),
]

# Insert the new cells right before the existing "## Plot 3 — StripedMap vs BronsonAVL"
# (which was at index 6). Renumber the existing Plot 3 → Plot 4.
nb["cells"][6]["source"] = ["## Plot 4 — StripedMap vs BronsonAVL, head-to-head per lock primitive\n"]
# Insert at position 6 (so new cells become 6 and 7, and old [6] becomes [8])
for i, c in enumerate(new_cells):
    nb["cells"].insert(6 + i, c)

# Update the 8-thread tables to include wormhole
for i, c in enumerate(nb["cells"]):
    src = "".join(c.get("source", []))
    if c["cell_type"] == "code" and src.startswith("def at8(df):"):
        c["source"] = [
            "def at8(df, locks):\n",
            "    s = df[df.threads == 8].pivot_table(index='workload', columns='lk',\n",
            "                                        values='ops_s', aggfunc='mean') / 1e6\n",
            "    return s[[c for c in locks if c in s.columns]]\n",
            "\n",
            "print('=== StripedMap, 8 threads, M ops/s ===')\n",
            "print(at8(cds, ['std','tas','ttas','cas','ticket']).round(1).to_string()); print()\n",
            "print('=== BronsonAVL, 8 threads, M ops/s ===')\n",
            "print(at8(avl, ['std','tas','ttas','cas','ticket']).round(1).to_string()); print()\n",
            "print('=== Wormhole,  8 threads, M ops/s ===')\n",
            "print(at8(wh,  ['default','rw','tas','ttas','cas','occ']).round(1).to_string())\n",
        ]
    if c["cell_type"] == "code" and src.startswith("def speedup_vs_std(df):"):
        c["source"] = [
            "def speedup_vs_baseline(df, locks, baseline):\n",
            "    t = df[df.threads == 8].pivot_table(index='workload', columns='lk',\n",
            "                                        values='ops_s', aggfunc='mean') / 1e6\n",
            "    return t[[c for c in locks if c in t.columns]].div(t[baseline], axis=0).round(2)\n",
            "\n",
            "print('=== StripedMap speedup vs cds-std (8 threads) ===')\n",
            "print(speedup_vs_baseline(cds, ['std','tas','ttas','cas','ticket'], 'std').to_string())\n",
            "print()\n",
            "print('=== BronsonAVL speedup vs avl-std (8 threads) ===')\n",
            "print(speedup_vs_baseline(avl, ['std','tas','ttas','cas','ticket'], 'std').to_string())\n",
            "print()\n",
            "print('=== Wormhole speedup vs wh-default (8 threads) ===')\n",
            "print(speedup_vs_baseline(wh,  ['default','rw','tas','ttas','cas','occ'], 'default').to_string())\n",
        ]

json.dump(nb, open(NB, "w"), indent=1)
print(f"Wrote {NB} ({len(nb['cells'])} cells)")
