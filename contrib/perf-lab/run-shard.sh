#!/bin/bash
# perf-lab: run ONE benchmark cell (a "shard") against one or two compilers.
#
#   run-shard.sh CELL TIP_BUILD [BASE_BUILD]
#
# One shard = one TU x flag cell. The workflow matrix supplies ALL
# parallelism; inside a shard everything is strictly serial on one pinned
# core -- compiles are never co-run. The timing protocol is run-suite.sh's,
# unchanged: 2 warmups (1 for huge) + median-of-7 (5 for huge), sides
# alternating run-by-run on the same core, /usr/bin/time -v per run.
#
# Cells (A/B when BASE_BUILD is given, tip-only otherwise):
#   light-O0 stl-O0 stl-O2 big-O2 realism-O0    median of 7
#   huge-O2 huge-O2-never                       median of 5
# Cells that are always tip-only (PCH ceiling, no-PCH vs PCH on tip):
#   pch-stl-O0 pch-stl-O0g pch-stl-O2 pch-json-O0 pch-json-O2
#   pch-big-O2 pch-anchor-O0                    median of 7
#
# Per-cell extras (only where that cell needs them):
#   huge-O2      /proc/vmstat thp_* counter deltas around one compile per side
#   realism-O0   strace -c syscall profile per side (openat/readlink counts)
#   pch-*        .gch built by driving cc1plus directly, verified via -H `!`
#                and .o byte-identity (except anchor, by design)
#
# Env: BENCH_DIR (required, gen-benches.sh output), RESULTS_DIR (default
# ./results), PIN_CPU (default 2), BENCH_REF (label only).
#
# TIP_BUILD/BASE_BUILD may be full build-gcc.sh trees OR unpacked
# pack-compiler.sh packages: a relative .perf-lab-src is resolved against
# the build dir itself.
#
# Outputs in RESULTS_DIR:
#   shard-summary.tsv  one row (see SUMHDR below) consumed by
#                      aggregate-shards.sh
#   perf-lab.tsv       raw per-run rows (same format as run-suite.sh)
#   tv.d/              /usr/bin/time -v stderr per run
#   plus the cell's extras (thp-delta-*.txt, strace.tsv + realism_*.strace,
#   gch-builds.tsv / pch-verify.tsv / H_*.txt)
#
# Honesty rule: if the two sides' min..max wall ranges overlap, the shard
# reports verdict NOISY instead of pretending the delta is significant.
set -euo pipefail

CELL=${1:?usage: run-shard.sh CELL TIP_BUILD [BASE_BUILD]}
TIP=${2:?usage: run-shard.sh CELL TIP_BUILD [BASE_BUILD]}
BASE=${3:-}

BENCH=${BENCH_DIR:?BENCH_DIR must point at gen-benches.sh output}
OUT=${RESULTS_DIR:-$PWD/results}
PIN=${PIN_CPU:-2}
REFLABEL=${BENCH_REF:-tip}
mkdir -p "$OUT" "$OUT/tv.d"
TSV=$OUT/perf-lab.tsv
: > "$TSV"
SUMMARY=${GITHUB_STEP_SUMMARY:-$OUT/summary.md}

command -v /usr/bin/time >/dev/null 2>&1 || {
  echo "::error::/usr/bin/time missing (apt package 'time')"; exit 1; }
test -x "$TIP/gcc/xg++" || { echo "::error::$TIP/gcc/xg++ missing"; exit 1; }
test -f "$BENCH/bench_stl.cpp" || {
  echo "::error::$BENCH does not look like gen-benches.sh output"; exit 1; }

say() {
  printf '%s\n' "$*"
  printf '%s\n' "$*" >> "$SUMMARY"
  if [ "$SUMMARY" != "$OUT/summary.md" ]; then
    printf '%s\n' "$*" >> "$OUT/summary.md"
  fi
}

# ---- cell table --------------------------------------------------------------
# TU (relative to BENCH), FLAGS, INC_MODE (none|bench|realism), N rounds,
# WARM count, EXTRA (none|thp|strace), PRE/PCHCELL for pch cells.
TU='' INC_MODE=none N=7 WARM=2 EXTRA=none PRE='' PCHCELL='' NOVERIFY=0
FLAGS=()
case $CELL in
  light-O0)      TU=bench_light.cpp; FLAGS=(-O0) ;;
  stl-O0)        TU=bench_stl.cpp;   FLAGS=(-O0) ;;
  stl-O2)        TU=bench_stl.cpp;   FLAGS=(-O2) ;;
  big-O2)        TU=big_tu.cpp;      FLAGS=(-O2); INC_MODE=bench ;;
  realism-O0)    TU=realism/realism_tu.cpp; FLAGS=(-O0); INC_MODE=realism
                 EXTRA=strace ;;
  huge-O2)       TU=huge_tu.cpp; FLAGS=(-O2); INC_MODE=bench; N=5; WARM=1
                 EXTRA=thp ;;
  huge-O2-never) TU=huge_tu.cpp; FLAGS=(-O2 --param ggc-min-expand=2147483647)
                 INC_MODE=bench; N=5; WARM=1 ;;
  pch-stl-O0)    TU=bench_stl.cpp;  FLAGS=(-O0);    PRE=stl;  PCHCELL=O0 ;;
  pch-stl-O0g)   TU=bench_stl.cpp;  FLAGS=(-O0 -g); PRE=stl;  PCHCELL=O0g ;;
  pch-stl-O2)    TU=bench_stl.cpp;  FLAGS=(-O2);    PRE=stl;  PCHCELL=O2 ;;
  pch-json-O0)   TU=bench_json.cpp; FLAGS=(-O0); INC_MODE=bench; PRE=json; PCHCELL=O0 ;;
  pch-json-O2)   TU=bench_json.cpp; FLAGS=(-O2); INC_MODE=bench; PRE=json; PCHCELL=O2 ;;
  pch-big-O2)    TU=big_tu.cpp;     FLAGS=(-O2); INC_MODE=bench; PRE=stl;  PCHCELL=O2 ;;
  pch-anchor-O0) TU=anchor.cpp;     FLAGS=(-O0);    PRE=stl;  PCHCELL=O0; NOVERIFY=1 ;;
  *) echo "::error::unknown cell: $CELL"; exit 2 ;;
esac

MODE=ab
if [ -n "$PRE" ]; then
  MODE=pch
  BASE=''   # pch cells are tip-only by definition
elif [ -z "$BASE" ]; then
  MODE=tiponly
fi

test -f "$BENCH/$TU" || { echo "::error::$BENCH/$TU missing"; exit 1; }

# ---- CPU pinning (identical to run-suite.sh) ---------------------------------
PINCMD=()
if command -v taskset >/dev/null 2>&1; then
  NC=$(nproc)
  if [ "$PIN" -ge "$NC" ]; then PIN=$((NC - 1)); fi
  if taskset -c "$PIN" true 2>/dev/null; then
    PINCMD=(taskset -c "$PIN")
  fi
fi

# ---- per-side compiler setup ---------------------------------------------------
side_build() { case $1 in tip) echo "$TIP" ;; base) echo "$BASE" ;; esac; }
resolve_src() { # BUILD -> absolute source/package src dir
  local b=$1 v
  v=$(cat "$b/.perf-lab-src")
  case $v in /*) printf '%s\n' "$v" ;; *) printf '%s/%s\n' "$b" "$v" ;; esac
}

# Triplet detection globs */libstdc++-v3/include (only the BUILT tree has
# include/ there) so the package's src/libstdc++-v3/libsupc++ stub can never
# shadow the real x86_64-*-linux-gnu dir.
triplet_of() { basename "$(dirname "$(dirname "$(ls -d "$1"/*/libstdc++-v3/include | head -n1)")")"; }

SRC_tip=$(resolve_src "$TIP")
T_tip=$(triplet_of "$TIP")
HDR_tip=(-nostdinc++
  -I"$TIP/$T_tip/libstdc++-v3/include/$T_tip"
  -I"$TIP/$T_tip/libstdc++-v3/include"
  -I"$SRC_tip/libstdc++-v3/libsupc++")
SRC_base=''
T_base=''
HDR_base=()
if [ -n "$BASE" ]; then
  test -x "$BASE/gcc/xg++" || { echo "::error::$BASE/gcc/xg++ missing"; exit 1; }
  SRC_base=$(resolve_src "$BASE")
  T_base=$(triplet_of "$BASE")
  HDR_base=(-nostdinc++
    -I"$BASE/$T_base/libstdc++-v3/include/$T_base"
    -I"$BASE/$T_base/libstdc++-v3/include"
    -I"$SRC_base/libstdc++-v3/libsupc++")
fi

# comp SIDE ARGS... : one compile, output passing through (verify/strace)
comp() {
  local sd=$1; shift
  local b; b=$(side_build "$sd")
  if [ "$sd" = tip ]; then
    "${PINCMD[@]}" "$b/gcc/xg++" -B"$b/gcc" "${HDR_tip[@]}" "$@"
  else
    "${PINCMD[@]}" "$b/gcc/xg++" -B"$b/gcc" "${HDR_base[@]}" "$@"
  fi
}

# timed SIDE LABEL ARGS... : one timed run appended to the TSV.
# Row: label \t side \t r$ROUND \t wall_s \t maxrss_kb \t minor_faults \t rc=N
ROUND=0
timed() {
  local sd=$1 label=$2; shift 2
  local b tv rc=0
  b=$(side_build "$sd")
  tv=$OUT/tv.d/$label.$sd.r$ROUND.tv
  if [ "$sd" = tip ]; then
    /usr/bin/time -v "${PINCMD[@]}" "$b/gcc/xg++" -B"$b/gcc" "${HDR_tip[@]}" "$@" \
      >/dev/null 2>"$tv" || rc=$?
  else
    /usr/bin/time -v "${PINCMD[@]}" "$b/gcc/xg++" -B"$b/gcc" "${HDR_base[@]}" "$@" \
      >/dev/null 2>"$tv" || rc=$?
  fi
  local wall rss minflt
  wall=$(awk -F': ' '/Elapsed \(wall clock\)/{n=split($2,a,":");
    if (n==3) print a[1]*3600+a[2]*60+a[3]; else print a[1]*60+a[2]}' "$tv")
  rss=$(awk -F': ' '/Maximum resident set size/{print $2}' "$tv")
  minflt=$(awk -F': ' '/Minor \(reclaiming a frame\) page faults/{print $2}' "$tv")
  printf '%s\t%s\tr%s\t%s\t%s\t%s\trc=%s\n' \
    "$label" "$sd" "$ROUND" "${wall:-nan}" "${rss:-0}" "${minflt:-0}" "$rc" >> "$TSV"
  if [ "$rc" -ne 0 ]; then
    echo "::warning::compile failed: $label/$sd rc=$rc (stderr in artifact tv.d/)"
  fi
}

# ---- stats helpers (rc=0 rows only; identical to run-suite.sh) ----------------
vals() { awk -F'\t' -v l="$1" -v s="$2" -v c="$3" \
  '$1==l && $2==s && $7=="rc=0" {print $c}' "$TSV"; }
med() { sort -g | awk '{a[NR]=$1} END{
  if (!NR) { print "nan"; exit }
  if (NR%2) printf "%.3f\n", a[(NR+1)/2];
  else printf "%.3f\n", (a[NR/2]+a[NR/2+1])/2 }'; }
mwall() { vals "$1" "$2" 4 | med; }
mrssmb() { vals "$1" "$2" 5 | med | awk '{ if ($1=="nan") print "n/a"; else printf "%.0f\n", $1/1024 }'; }
mflt() { vals "$1" "$2" 6 | med | awk '{ if ($1=="nan") print "n/a"; else printf "%.0f\n", $1 }'; }
wrange() { vals "$1" "$2" 4 | sort -g | awk 'NR==1{mn=$1} {mx=$1} END{
  if (!NR) print "n/a"; else printf "%.2f..%.2f\n", mn, mx }'; }
nok() { vals "$1" "$2" 4 | wc -l | tr -d ' '; }
pct() { awk -v a="$1" -v b="$2" 'BEGIN{
  if (a=="nan" || b=="nan" || a=="n/a" || b=="n/a" || b+0==0) { print "n/a"; exit }
  printf "%+.1f%%", (a-b)/b*100 }'; }
ratio() { awk -v a="$1" -v b="$2" 'BEGIN{
  if (a=="nan" || b=="nan" || b+0==0) { print "n/a"; exit }
  printf "%.2fx", a/b }'; }
# ranges_overlap A_LABEL A_SIDE B_LABEL B_SIDE -> yes|no|n/a
ranges_overlap() {
  { vals "$1" "$2" 4 | sort -g | awk 'NR==1{mn=$1} {mx=$1} END{if (NR) print mn, mx}'
    vals "$3" "$4" 4 | sort -g | awk 'NR==1{mn=$1} {mx=$1} END{if (NR) print mn, mx}'
  } | awk 'NR==1{amn=$1; amx=$2} NR==2{bmn=$1; bmx=$2} END{
      if (NR<2) { print "n/a"; exit }
      if (amn <= bmx && bmn <= amx) print "yes"; else print "no" }'
}

# ---- environment header ------------------------------------------------------
LOAD0=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null || echo '?')
say "## shard \`$CELL\` (mode: $MODE)"
say ""
say "- date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "- runner: \`${RUNNER_NAME:-?}\` on \`$(hostname 2>/dev/null || echo '?')\`, $(nproc) CPUs, $(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo '?') RAM, loadavg $LOAD0"
say "- cpu: $(lscpu 2>/dev/null | awk -F': +' '/Model name/{print $2; exit}' || echo unknown)"
say "- THP: enabled=\`$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo n/a)\` defrag=\`$(cat /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || echo n/a)\`"
say "- pin: ${PINCMD[*]:-none (taskset unavailable)}"
say "- tip ($REFLABEL): \`$(cat "$TIP/.perf-lab-sha")\` -- $("$TIP/gcc/xg++" --version 2>/dev/null | head -n1)"
if [ -n "$BASE" ]; then
  say "- base: \`$(cat "$BASE/.perf-lab-sha")\` -- $("$BASE/gcc/xg++" --version 2>/dev/null | head -n1)"
fi
say ""

# ---- include args for this cell ----------------------------------------------
INC=()
case $INC_MODE in
  bench)   INC=(-I"$BENCH") ;;
  realism) for d in "$BENCH"/realism/roots/r*; do INC+=(-I"$d"); done ;;
esac

# ---- extras ------------------------------------------------------------------
thp_probe() { # SIDE: /proc/vmstat thp_* delta around one huge_tu -O2 compile
  local sd=$1
  local f=$OUT/thp_$sd
  grep -E '^thp_(fault_alloc|fault_fallback|collapse_alloc) ' /proc/vmstat \
    > "$f.before" 2>/dev/null || { echo "no /proc/vmstat thp counters"; return 0; }
  comp "$sd" -O2 -I"$BENCH" -c "$BENCH/huge_tu.cpp" -o /dev/null || true
  grep -E '^thp_(fault_alloc|fault_fallback|collapse_alloc) ' /proc/vmstat \
    > "$f.after" 2>/dev/null || return 0
  local delta
  delta=$(paste "$f.before" "$f.after" | awk '{printf "%-22s +%d\n", $1, $4-$2}')
  printf '%s\n' "$delta" > "$OUT/thp-delta-$sd.txt"
  say ""
  say "THP counter delta around one huge_tu -O2 compile (**$sd**):"
  say '```'
  say "$delta"
  say '```'
}

strace_probe() { # syscall profile on the realism TU per side
  command -v strace >/dev/null 2>&1 || {
    say ""; say "(strace not present -- syscall table skipped)"; return 0; }
  local STRACETSV=$OUT/strace.tsv
  printf 'side\ttotal\topenat\treadlink\n' > "$STRACETSV"
  say ""
  say "### strace -c on realism-O0 (cc1plus via -wrapper)"
  say ""
  say "| side | total syscalls | openat | readlink |"
  say "|---|---|---|---|"
  local sd
  for sd in "${SIDES[@]}"; do
    comp "$sd" "${INC[@]}" -wrapper strace,-f,-c,-o,"$OUT/realism_$sd.strace" \
      -O0 -c "$BENCH/realism/realism_tu.cpp" -o /dev/null || true
    if [ -f "$OUT/realism_$sd.strace" ]; then
      local tot op rl
      tot=$(awk '/^100\.00/{print $4; exit}' "$OUT/realism_$sd.strace")
      op=$(awk '$NF=="openat"{print $4; exit}' "$OUT/realism_$sd.strace")
      rl=$(awk '$NF=="readlink"{print $4; exit}' "$OUT/realism_$sd.strace")
      say "| $sd | ${tot:-?} | ${op:-0} | ${rl:-0} |"
      printf '%s\t%s\t%s\t%s\n' "$sd" "${tot:-?}" "${op:-0}" "${rl:-0}" >> "$STRACETSV"
    else
      say "| $sd | (strace produced no output) | | |"
    fi
  done
}

# ---- pch machinery (tip only; ported from run-suite.sh suite_pch) -------------
pch_setup() {
  test -x "$TIP/gcc/cc1plus" || { echo "::error::$TIP/gcc/cc1plus missing"; exit 1; }
  P=$BENCH/pch
  rm -rf "$P"
  GCHTSV=$OUT/gch-builds.tsv
  printf 'prelude\tcell\tbuild_s\tgch_MB\trc\n' > "$GCHTSV"

  # .gch built by driving cc1plus DIRECTLY: the fork's driver breaks
  # `-x c++-header` (duplicate -o from the integrated-as single-process
  # change). PCH *use* goes through the driver as normal.
  local d=$P/$PCHCELL
  mkdir -p "$d"
  cp "$BENCH/prelude_$PRE.h" "$d/"
  local XI=()
  if [ "$PRE" = json ]; then XI=(-I "$BENCH"); fi
  local t0 t1 rc=0 sz
  t0=$(date +%s.%N)
  "${PINCMD[@]}" "$TIP/gcc/cc1plus" -quiet -nostdinc++ \
    -I "$TIP/$T_tip/libstdc++-v3/include/$T_tip" \
    -I "$TIP/$T_tip/libstdc++-v3/include" \
    -I "$SRC_tip/libstdc++-v3/libsupc++" \
    "${XI[@]}" \
    -isystem "$TIP/gcc/include" -isystem "$TIP/gcc/include-fixed" \
    -D_GNU_SOURCE -imultiarch x86_64-linux-gnu \
    "$d/prelude_$PRE.h" -dumpbase "prelude_$PRE.h" -dumpbase-ext .h \
    -mtune=generic -march=x86-64 "${FLAGS[@]}" \
    -o /dev/null --output-pch "$d/prelude_$PRE.h.gch" \
    2> "$OUT/gch-build.err" || rc=$?
  t1=$(date +%s.%N)
  sz=$(stat -c %s "$d/prelude_$PRE.h.gch" 2>/dev/null || echo 0)
  printf '%s\t%s\t%s\t%s\trc=%s\n' "$PRE" "$PCHCELL" \
    "$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')" \
    "$(awk -v s="$sz" 'BEGIN{printf "%.1f", s/1048576}')" "$rc" >> "$GCHTSV"
  if [ "$rc" -ne 0 ]; then
    echo "::error::gch build failed: $PRE/$PCHCELL (rc=$rc)"
    head -20 "$OUT/gch-build.err" || true
    exit 1
  fi
  say "- .gch: prelude_$PRE/$PCHCELL, $(awk -F'\t' 'NR==2{print $3}' "$GCHTSV") s to build, $(awk -F'\t' 'NR==2{print $4}' "$GCHTSV") MB"
}

pch_verify() { # -H '!' marker + .o byte identity for this cell
  VER=$OUT/pch-verify.tsv
  printf 'label\thbang\tidentical\n' > "$VER"
  [ "$NOVERIFY" = 1 ] && {
    say "- verify: skipped by design (anchor: with -include the token stream differs)"
    return 0; }
  local tu=$BENCH/$TU hn=0 ident=SKIP
  comp tip "${INC[@]}" "${FLAGS[@]}" -H -Winvalid-pch \
    -include "$P/$PCHCELL/prelude_$PRE.h" \
    -fsyntax-only "$tu" 2> "$OUT/H_$CELL.txt" || true
  hn=$(grep -c '^!' "$OUT/H_$CELL.txt" 2>/dev/null || true)
  if comp tip "${INC[@]}" "${FLAGS[@]}" -c "$tu" -o "$OUT/id.nopch.o" \
     && comp tip "${INC[@]}" "${FLAGS[@]}" -Winvalid-pch \
          -include "$P/$PCHCELL/prelude_$PRE.h" -c "$tu" -o "$OUT/id.pch.o"; then
    if cmp -s "$OUT/id.nopch.o" "$OUT/id.pch.o"; then ident=yes; else ident=NO; fi
  else
    ident=compile-failed
  fi
  rm -f "$OUT/id.nopch.o" "$OUT/id.pch.o"
  printf '%s\t%s\t%s\n' "$CELL" "$hn" "$ident" >> "$VER"
  say "- verify: -H '!' lines=$hn, .o identical=$ident"
}

# =============================== measurement ==================================
SIDES=(tip)
[ "$MODE" = ab ] && SIDES=(base tip)

LAB=$CELL
A_LABEL='' A_SIDE='' B_LABEL='' B_SIDE=''

if [ "$MODE" = pch ]; then
  pch_setup
  pch_verify
  LAB=${CELL#pch-}
  local_tu=$BENCH/$TU
  # interleaved no-PCH / PCH pairs, tip compiler only
  for w in $(seq 1 "$WARM"); do
    ROUND=w$w
    timed tip "nopch-warm-$LAB" "${INC[@]}" "${FLAGS[@]}" -c "$local_tu" -o /dev/null
    timed tip "pch-warm-$LAB" "${INC[@]}" "${FLAGS[@]}" -Winvalid-pch \
      -include "$P/$PCHCELL/prelude_$PRE.h" -c "$local_tu" -o /dev/null
  done
  for i in $(seq 1 "$N"); do
    ROUND=$i
    timed tip "nopch-$LAB" "${INC[@]}" "${FLAGS[@]}" -c "$local_tu" -o /dev/null
    timed tip "pch-$LAB" "${INC[@]}" "${FLAGS[@]}" -Winvalid-pch \
      -include "$P/$PCHCELL/prelude_$PRE.h" -c "$local_tu" -o /dev/null
    echo "pch pair round $i/$N done"
  done
  A_LABEL=nopch-$LAB; A_SIDE=tip   # "A" column = no-PCH
  B_LABEL=pch-$LAB;   B_SIDE=tip   # "B" column = PCH
else
  for w in $(seq 1 "$WARM"); do
    ROUND=w$w
    for sd in "${SIDES[@]}"; do
      timed "$sd" "warm-$CELL" "${INC[@]}" "${FLAGS[@]}" -c "$BENCH/$TU" -o /dev/null
    done
  done
  for i in $(seq 1 "$N"); do
    ROUND=$i
    for sd in "${SIDES[@]}"; do
      timed "$sd" "$CELL" "${INC[@]}" "${FLAGS[@]}" -c "$BENCH/$TU" -o /dev/null
    done
    echo "round $i/$N done ($(uptime 2>/dev/null | sed 's/.*load/load/' || true))"
  done
  if [ "$MODE" = ab ]; then
    A_LABEL=$CELL; A_SIDE=base
    B_LABEL=$CELL; B_SIDE=tip
  else
    A_LABEL=''; A_SIDE=''
    B_LABEL=$CELL; B_SIDE=tip
  fi
  case $EXTRA in
    thp)    for sd in "${SIDES[@]}"; do thp_probe "$sd"; done ;;
    strace) strace_probe ;;
  esac
fi

# =============================== verdict ======================================
# A = base (or no-PCH), B = tip (or PCH). ratio = A/B (>1 => B faster).
B_MED=$(mwall "$B_LABEL" "$B_SIDE")
B_RANGE=$(wrange "$B_LABEL" "$B_SIDE")
B_N=$(nok "$B_LABEL" "$B_SIDE")
B_FLT=$(mflt "$B_LABEL" "$B_SIDE")
B_RSS=$(mrssmb "$B_LABEL" "$B_SIDE")
if [ -n "$A_LABEL" ]; then
  A_MED=$(mwall "$A_LABEL" "$A_SIDE")
  A_RANGE=$(wrange "$A_LABEL" "$A_SIDE")
  A_N=$(nok "$A_LABEL" "$A_SIDE")
  A_FLT=$(mflt "$A_LABEL" "$A_SIDE")
  A_RSS=$(mrssmb "$A_LABEL" "$A_SIDE")
else
  A_MED=- A_RANGE=- A_N=0 A_FLT=- A_RSS=-
fi

VERDICT=n/a
RATIO=-
DELTA=-
if [ -n "$A_LABEL" ]; then
  RATIO=$(ratio "$A_MED" "$B_MED")
  DELTA=$(pct "$B_MED" "$A_MED")
  if [ "$A_N" = 0 ] || [ "$B_N" = 0 ]; then
    VERDICT=FAILED
  elif [ "$(ranges_overlap "$A_LABEL" "$A_SIDE" "$B_LABEL" "$B_SIDE")" = yes ]; then
    VERDICT=NOISY
  else
    faster=$(awk -v a="$A_MED" -v b="$B_MED" 'BEGIN{
      if (b+0 < a+0) print "B"; else if (a+0 < b+0) print "A"; else print "tie"}')
    if [ "$MODE" = pch ]; then
      case $faster in B) VERDICT=pch-faster ;; A) VERDICT=pch-slower ;; *) VERDICT=~same ;; esac
    else
      case $faster in B) VERDICT=tip-faster ;; A) VERDICT=tip-slower ;; *) VERDICT=~same ;; esac
    fi
  fi
elif [ "$B_N" = 0 ]; then
  VERDICT=FAILED
fi

LOAD1=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null || echo '?')

# summary row for aggregate-shards.sh
SUMHDR='cell\tmode\tn_a\tn_b\ta_med_s\tb_med_s\tratio\tdelta\ta_range\tb_range\ta_minflt\tb_minflt\ta_rss_mb\tb_rss_mb\tverdict\tload\n'
SUM=$OUT/shard-summary.tsv
printf '%b' "$SUMHDR" > "$SUM"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$CELL" "$MODE" "$A_N" "$B_N" "$A_MED" "$B_MED" "$RATIO" "$DELTA" \
  "$A_RANGE" "$B_RANGE" "$A_FLT" "$B_FLT" "$A_RSS" "$B_RSS" "$VERDICT" \
  "$LOAD0->$LOAD1" >> "$SUM"

ACOL='base'; BCOL='tip'
if [ "$MODE" = pch ]; then ACOL='no-PCH'; BCOL='PCH'; fi
say ""
say "| cell | $ACOL med (s) | $BCOL med (s) | ratio | $ACOL range | $BCOL range | n | $ACOL minflt | $BCOL minflt | verdict |"
say "|---|---|---|---|---|---|---|---|---|---|"
say "| $CELL | $A_MED | $B_MED | $RATIO | $A_RANGE | $B_RANGE | $A_N/$B_N | $A_FLT | $B_FLT | **$VERDICT** |"
say ""
say "loadavg $LOAD0 -> $LOAD1; raw rows in \`perf-lab.tsv\`, \`/usr/bin/time -v\` in \`tv.d/\` (artifact \`shard-$CELL\`)."
if [ "$VERDICT" = NOISY ]; then
  say ""
  say "NOISY: the two sides' min..max wall ranges overlap -- this shard does not claim a direction."
fi
echo "SHARD $CELL COMPLETE verdict=$VERDICT"
# A shard with zero usable runs on a side is a broken shard, not a result:
# exit nonzero so the job goes red instead of hollow-green (the results
# artifact still uploads -- that workflow step runs on always()). NOISY is
# an honest result and stays green.
if [ "$VERDICT" = FAILED ]; then
  echo "::error::shard $CELL produced no usable runs on at least one side (see tv.d/ in the shard artifact)"
  exit 1
fi
