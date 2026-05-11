#!/usr/bin/env python3
"""
aggregate.py — Aggregate per-trial CSV rows into median / IQR / CoV summaries.

Reads the raw CSV produced by lockbench / wh_bench / index_bench (semicolon-separated,
European decimal comma) and emits an aggregated CSV grouped by configuration. Useful
when each (lock, threads, workload) cell has multiple trials.

Usage:
    python3 scripts/aggregate.py <input.csv> [--out out.csv] [--cov-warn 10]

Grouping keys are auto-detected from the columns; metric column is `ops_s` (default,
the throughput field used in all three CSV schemas), with `ns_op` as a secondary metric.

Notes:
- Numeric values use `,` as the decimal separator (matches `fmt_double` in util.hpp).
- Prints a warning to stderr for any group whose CoV exceeds --cov-warn (default 10%).
"""

import argparse
import csv
import math
import statistics
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


# Columns that are NOT grouping keys (they are the per-trial metrics or labels).
NON_KEY_COLUMNS = {
    "total_ops", "read_ops", "write_ops", "ops_s", "ns_op",
    "fairness_min", "fairness_max", "fairness_ratio",
    "gets", "puts", "removes",
    "hostname",  # not a config dimension; report separately
    "seconds",
}

# Columns to aggregate as numeric metrics.
METRICS = ["ops_s", "ns_op", "fairness_ratio"]


def parse_num(s: str) -> Optional[float]:
    """Parse a number that may use ',' as the decimal separator. Returns None on fail."""
    if s is None or s == "":
        return None
    try:
        return float(s.replace(",", "."))
    except ValueError:
        return None


def fmt_num(v: Optional[float]) -> str:
    if v is None:
        return ""
    if math.isnan(v):
        return ""
    return f"{v:.6g}".replace(".", ",")


def cov_pct(values: List[float]) -> Optional[float]:
    """Coefficient of variation (stddev / mean) as a percentage. None if undefined."""
    if len(values) < 2:
        return None
    m = statistics.fmean(values)
    if m == 0:
        return None
    sd = statistics.stdev(values)
    return 100.0 * sd / m


def aggregate(input_path: Path, output_path: Path, cov_warn: float) -> int:
    with input_path.open(newline="") as f:
        reader = csv.DictReader(f, delimiter=";")
        if reader.fieldnames is None:
            print(f"error: {input_path} has no header", file=sys.stderr)
            return 2
        all_cols = list(reader.fieldnames)
        rows = list(reader)

    if not rows:
        print(f"error: {input_path} contains no data rows", file=sys.stderr)
        return 2

    # Grouping = all non-metric, non-label columns.
    key_cols = [c for c in all_cols if c not in NON_KEY_COLUMNS]
    metric_cols = [c for c in METRICS if c in all_cols]
    if not metric_cols:
        print(f"error: no aggregatable metric column found (looked for {METRICS})", file=sys.stderr)
        return 2

    groups: Dict[Tuple, List[Dict]] = {}
    for row in rows:
        key = tuple(row.get(c, "") for c in key_cols)
        groups.setdefault(key, []).append(row)

    # Emit aggregated rows.
    out_cols = list(key_cols) + ["n"]
    for m in metric_cols:
        out_cols.extend([f"{m}_median", f"{m}_mean", f"{m}_p25", f"{m}_p75",
                         f"{m}_min", f"{m}_max", f"{m}_cov_pct"])

    high_cov = []
    with output_path.open("w", newline="") as f:
        writer = csv.writer(f, delimiter=";")
        writer.writerow(out_cols)

        for key, group_rows in sorted(groups.items()):
            out_row = list(key) + [str(len(group_rows))]
            for m in metric_cols:
                vals = [parse_num(r.get(m, "")) for r in group_rows]
                vals = [v for v in vals if v is not None]
                if not vals:
                    out_row.extend([""] * 7)
                    continue
                vals_sorted = sorted(vals)
                med = statistics.median(vals_sorted)
                mean = statistics.fmean(vals_sorted)
                p25 = statistics.quantiles(vals_sorted, n=4)[0] if len(vals_sorted) >= 4 \
                      else vals_sorted[0]
                p75 = statistics.quantiles(vals_sorted, n=4)[2] if len(vals_sorted) >= 4 \
                      else vals_sorted[-1]
                cov = cov_pct(vals_sorted)
                out_row.extend([
                    fmt_num(med), fmt_num(mean), fmt_num(p25), fmt_num(p75),
                    fmt_num(vals_sorted[0]), fmt_num(vals_sorted[-1]),
                    fmt_num(cov) if cov is not None else "",
                ])
                if m == "ops_s" and cov is not None and cov > cov_warn:
                    high_cov.append((key, cov, len(vals_sorted)))
            writer.writerow(out_row)

    print(f"wrote {len(groups)} aggregated rows → {output_path}")
    if high_cov:
        print(f"\n[WARNING] {len(high_cov)} groups exceed CoV threshold ({cov_warn}%):",
              file=sys.stderr)
        for key, cov, n in sorted(high_cov, key=lambda x: -x[1])[:20]:
            kv = ", ".join(f"{k}={v}" for k, v in zip(key_cols, key) if v)
            print(f"  CoV={cov:.1f}% n={n}  {kv}", file=sys.stderr)
        if len(high_cov) > 20:
            print(f"  ... and {len(high_cov) - 20} more", file=sys.stderr)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path, help="raw per-trial CSV")
    ap.add_argument("--out", type=Path, default=None,
                    help="output path (default: alongside input with .agg.csv)")
    ap.add_argument("--cov-warn", type=float, default=10.0,
                    help="warn for groups whose ops_s CoV exceeds this percentage")
    args = ap.parse_args()
    if not args.input.exists():
        print(f"error: {args.input} not found", file=sys.stderr)
        return 2
    out = args.out or args.input.with_suffix(".agg.csv")
    return aggregate(args.input, out, args.cov_warn)


if __name__ == "__main__":
    sys.exit(main())
