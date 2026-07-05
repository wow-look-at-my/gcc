#!/bin/bash
# perf-lab: merge per-shard results into the final table.
#
#   aggregate-shards.sh SHARDS_IN OUTDIR
#
# SHARDS_IN is the download dir of all shard-* artifacts (one subdirectory
# per shard, each containing that shard's shard-summary.tsv plus raw files).
#
# Env:
#   EXPECTED_CELLS  space-separated cell list in canonical order (the
#                   workflow passes the same list the shards matrix ran);
#                   cells with no summary are reported as MISSING
#   SUITE           label only
#   GITHUB_STEP_SUMMARY  markdown table target (falls back to OUTDIR file)
#
# Produces in OUTDIR: merged-summary.tsv, final-summary.md, and a shards/
# copy of every shard's raw files, so the single `perf-lab-results` artifact
# is a one-stop download.
#
# Verdict columns come straight from the shards. On top of that, each cell
# is checked against the expected direction of this branch's merged wins
# (see contrib/perf-lab/README.md): big/huge/stl/realism cells expect tip
# faster; light-O0 is the no-change control; pch cells expect a PCH speedup
# (anchor is the load-cost floor, either direction fine); realism expects a
# syscall (openat/readlink) drop on tip. Contradictions are flagged
# SURPRISE; NOISY shards are flagged no-signal, never SURPRISE.
set -euo pipefail

IN=${1:?usage: aggregate-shards.sh SHARDS_IN OUTDIR}
OUT=${2:?usage: aggregate-shards.sh SHARDS_IN OUTDIR}
mkdir -p "$OUT"
SUMMARY=${GITHUB_STEP_SUMMARY:-$OUT/final-summary.md}

say() {
  printf '%s\n' "$*"
  printf '%s\n' "$*" >> "$SUMMARY"
  if [ "$SUMMARY" != "$OUT/final-summary.md" ]; then
    printf '%s\n' "$*" >> "$OUT/final-summary.md"
  fi
}

# ---- collect ------------------------------------------------------------------
MERGED=$OUT/merged-summary.tsv
printf 'cell\tmode\tn_a\tn_b\ta_med_s\tb_med_s\tratio\tdelta\ta_range\tb_range\ta_minflt\tb_minflt\ta_rss_mb\tb_rss_mb\tverdict\tload\n' > "$MERGED"

# cells that actually arrived, keyed by cell name
declare -A ROWS
found=0
for f in "$IN"/shard-*/shard-summary.tsv; do
  [ -f "$f" ] || continue
  row=$(awk -F'\t' 'NR==2' "$f")
  [ -n "$row" ] || continue
  cell=${row%%$'\t'*}
  ROWS[$cell]=$row
  found=$((found + 1))
done

if [ "$found" -eq 0 ]; then
  say "## perf-lab aggregate: NO shard summaries found in $IN"
  say ""
  say "Every shard either failed before writing shard-summary.tsv or uploaded nothing."
  exit 1
fi

CELLS=${EXPECTED_CELLS:-}
if [ -z "$CELLS" ]; then
  CELLS=$(printf '%s\n' "${!ROWS[@]}" | sort | tr '\n' ' ')
fi

# expected direction per cell (see header comment)
expect_for() { # CELL MODE -> expectation string
  local cell=$1 mode=$2
  if [ "$mode" = tiponly ]; then echo "n/a"; return; fi
  case $cell in
    light-O0)       echo "~same (control)" ;;
    pch-anchor-O0)  echo "any (PCH-load floor)" ;;
    pch-*)          echo "pch-faster" ;;
    stl-*|big-*|huge-*|realism-*) echo "tip-faster" ;;
    *)              echo "n/a" ;;
  esac
}

flag_for() { # CELL MODE VERDICT DELTA -> ok|SURPRISE|no-signal|FAILED|-
  local cell=$1 mode=$2 verdict=$3 delta=$4
  case $verdict in
    MISSING|FAILED) echo FAILED; return ;;
    NOISY)          echo no-signal; return ;;
    n/a)            echo -; return ;;
  esac
  case $(expect_for "$cell" "$mode") in
    "tip-faster")
      [ "$verdict" = tip-faster ] && echo ok || echo SURPRISE ;;
    "pch-faster")
      [ "$verdict" = pch-faster ] && echo ok || echo SURPRISE ;;
    "~same (control)")
      # control cell: any confident delta beyond +/-3% is a surprise
      awk -v d="$delta" 'BEGIN{
        gsub(/[+%]/, "", d)
        if (d+0 <= 3 && d+0 >= -3) print "ok"; else print "SURPRISE" }' ;;
    *) echo - ;;
  esac
}

# ---- render -------------------------------------------------------------------
say "# perf-lab results -- suite=\`${SUITE:-?}\` (sharded pipeline)"
say ""
say "- date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "- shards found: $found"
say "- columns: A = base compiler (or no-PCH for pch cells), B = tip (or PCH);"
say "  ratio = A/B, >1.00x means B (tip/PCH) is faster. Each shard ran its own"
say "  interleaved A/B on one pinned core of its own runner; NOISY = that"
say "  shard's two sides' min..max ranges overlap, so no direction is claimed."
say ""
say "| cell | A med (s) | B med (s) | ratio | delta | A range | B range | A minflt | B minflt | verdict | expected | flag |"
say "|---|---|---|---|---|---|---|---|---|---|---|---|"

surprises=0
missing=0
for cell in $CELLS; do
  if [ -n "${ROWS[$cell]:-}" ]; then
    row=${ROWS[$cell]}
    printf '%s\n' "$row" >> "$MERGED"
    IFS=$'\t' read -r c mode _na _nb amed bmed ratio delta arange brange aflt bflt _arss _brss verdict _load <<< "$row"
    exp=$(expect_for "$c" "$mode")
    flag=$(flag_for "$c" "$mode" "$verdict" "$delta")
    [ "$flag" = SURPRISE ] && surprises=$((surprises + 1))
    say "| $c | $amed | $bmed | $ratio | $delta | $arange | $brange | $aflt | $bflt | $verdict | $exp | $flag |"
  else
    missing=$((missing + 1))
    say "| $cell | - | - | - | - | - | - | - | - | **MISSING** | $(expect_for "$cell" ab) | FAILED |"
  fi
done

# realism syscall expectation: tip should open fewer files / readlink less
STRACE=$IN/shard-realism-O0/strace.tsv
if [ -f "$STRACE" ]; then
  say ""
  say "### realism-O0 syscall profile (strace -c, cc1plus)"
  say ""
  say "| side | total | openat | readlink |"
  say "|---|---|---|---|"
  awk -F'\t' 'NR>1{printf "| %s | %s | %s | %s |\n", $1,$2,$3,$4}' "$STRACE" \
    | while IFS= read -r line; do say "$line"; done
  sc=$(awk -F'\t' '
    NR>1 && $1=="base" {bt=$2; bo=$3; br=$4}
    NR>1 && $1=="tip"  {tt=$2; to=$3; tr=$4}
    END{
      if (bt=="" || tt=="") { print "one-sided"; exit }
      if (tt+0 < bt+0 && tr+0 <= br+0) print "ok"; else print "SURPRISE" }' "$STRACE")
  if [ "$sc" = SURPRISE ]; then
    surprises=$((surprises + 1))
    say ""
    say "**SURPRISE**: expected a tip syscall drop (dir-index/readlink wins) and did not see one."
  elif [ "$sc" = ok ]; then
    say ""
    say "syscall drop on tip: as expected."
  fi
fi

# THP deltas from the huge-O2 shard, verbatim
for side in base tip; do
  f=$IN/shard-huge-O2/thp-delta-$side.txt
  if [ -f "$f" ]; then
    say ""
    say "THP counter delta around one huge_tu -O2 compile (**$side**, from shard huge-O2):"
    say '```'
    while IFS= read -r line; do say "$line"; done < "$f"
    say '```'
  fi
done

# PCH verification roll-up
pchv=0
for f in "$IN"/shard-pch-*/pch-verify.tsv; do
  [ -f "$f" ] || continue
  if [ "$pchv" -eq 0 ]; then
    say ""
    say "### PCH verification (per shard)"
    say ""
    say "| cell | -H '!' lines | .o identical |"
    say "|---|---|---|"
    pchv=1
  fi
  awk -F'\t' 'NR>1{printf "| %s | %s | %s |\n", $1,$2,$3}' "$f" \
    | while IFS= read -r line; do say "$line"; done
done

say ""
if [ "$missing" -gt 0 ]; then
  say "**$missing shard(s) MISSING** -- see the failed shard job logs."
fi
if [ "$surprises" -gt 0 ]; then
  say "**$surprises SURPRISE flag(s)** -- a confident result contradicts the expected direction."
else
  say "No surprises: every confident result matches the expected direction."
fi
say ""
say "Merged TSV: \`merged-summary.tsv\`; every shard's raw files are under \`shards/\` in the \`perf-lab-results\` artifact."

# one-stop artifact: copy every shard's raw dir alongside the merged table
mkdir -p "$OUT/shards"
cp -a "$IN"/. "$OUT/shards/" 2>/dev/null || true

echo "AGGREGATE COMPLETE: $found shard(s), $missing missing, $surprises surprise(s)"
