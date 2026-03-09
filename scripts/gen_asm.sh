#!/usr/bin/env bash
# gen_asm.sh - generate annotated assembly for all lock primitives
# Usage: ./scripts/gen_asm.sh [output_dir]
#   default output_dir: asm/

set -euo pipefail

OUT_DIR="${1:-asm}"
SRC="bench/lock_asm.cpp"

if [ ! -f "$SRC" ]; then
  echo "Error: $SRC not found. Run from project root."
  exit 1
fi

mkdir -p "$OUT_DIR"

CXX="${CXX:-clang++}"
CXXFLAGS="-std=c++20 -O3 -march=native -Iinclude"

# full assembly file
echo "Generating $OUT_DIR/lock_asm.s ..."
$CXX $CXXFLAGS -S -o "$OUT_DIR/lock_asm.s" "$SRC"

# per-lock split files (strip directives for readability)
for fn in tas_lock ttas_lock ttas_unlock cas_lock cas_unlock \
          ticket_lock ticket_unlock \
          rw_read_lock rw_read_unlock rw_write_lock rw_write_unlock \
          occ_write_lock occ_write_unlock occ_read_begin occ_read_validate \
          tas_unlock; do
  # match the mangled name pattern
  pattern="${fn}_fn"
  outfile="$OUT_DIR/${fn}.s"
  # extract from the target label to the next top-level symbol label, while
  # keeping local block labels like LBB... inside the function body.
  awk -v pat="$pattern" '
    BEGIN { found=0 }
    {
      line = $0
      sub(/^[[:space:]]*/, "", line)

      if (line ~ /^[.$_[:alpha:]][.$_[:alnum:]]*:/) {
        label = line
        sub(/:.*/, "", label)

        if (!found) {
          if (index(label, pat)) found=1
        } else if (!index(label, pat) && label !~ /^\.?L/) {
          exit
        }
      }

      if (found) print
    }
  ' "$OUT_DIR/lock_asm.s" | grep -v '^\s*\.' | grep -v '^\s*;' | grep -v '^$' > "$outfile" 2>/dev/null || true

  if [ -s "$outfile" ]; then
    echo "  $outfile"
  else
    rm -f "$outfile"
  fi
done

echo ""
echo "Done. Full assembly: $OUT_DIR/lock_asm.s"
echo "Individual files in $OUT_DIR/"
ls -1 "$OUT_DIR"/*.s 2>/dev/null
