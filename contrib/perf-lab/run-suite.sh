#!/bin/bash
# perf-lab: run a benchmark suite against one or two built compilers.
#
#   run-suite.sh SUITE TIP_BUILD [BASE_BUILD]
#
#   SUITE       quick | composed | pch | full
#   TIP_BUILD   build dir from build-gcc.sh for the benchmarked ref
#   BASE_BUILD  build dir for the pre-optimization base (composed/full)
#
# Env: BENCH_DIR (required, gen-benches.sh output), RESULTS_DIR (default
# ./results), PIN_CPU (default 2), BENCH_REF (label only).
#
# Timing protocol (ported from the merged branch's measurement scratchpad):
#   - CPU-pinned via taskset where available
#   - 2 warmup runs + median of 7 (5 for huge_tu)
#   - base/tip interleaved per round to neutralize load drift
#   - /usr/bin/time -v for wall + max RSS + minor faults on every run
#   - strace -c where strace exists
#   - THP evidence: /proc/vmstat thp_* counter deltas around a huge_tu compile
#     plus per-run minor-fault medians (the MADV_HUGEPAGE win shows up as a
#     minor-fault collapse on a THP=madvise/always host)
#   - PCH: .gch built by driving cc1plus DIRECTLY (the fork's driver breaks
#     `-x c++-header` -- duplicate -o from the integrated-as single-process
#     change), use verified via -H's `!` marker, correctness via .o byte
#     identity against the no-PCH compile.
set -euo pipefail

SUITE=${1:?usage: run-suite.sh SUITE TIP_BUILD [BASE_BUILD]}
TIP=${2:?usage: run-suite.sh SUITE TIP_BUILD [BASE_BUILD]}
BASE=${3:-}

BENCH=${BENCH_DIR:?BENCH_DIR must point at gen-benches.sh output}
OUT=${RESULTS_DIR:-$PWD/results}
PIN=${PIN_CPU:-2}
REFLABEL=${BENCH_REF:-tip}
mkdir -p "$OUT" "$OUT/tv.d"
TSV=$OUT/perf-lab.tsv
: > "$TSV"
SUMMARY=${GITHUB_STEP_SUMMARY:-$OUT/summary.md}

case $SUITE in
  quick|composed|pch|full) ;;
  *) echo "unknown suite: $SUITE (want quick|composed|pch|full)"; exit 2 ;;
esac
if { [ "$SUITE" = composed ] || [ "$SUITE" = full ]; } && [ -z "$BASE" ]; then
  echo "suite $SUITE needs BASE_BUILD as third argument"; exit 2
fi
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

# ---- CPU pinning -------------------------------------------------------------
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

SRC_tip=$(cat "$TIP/.perf-lab-src")
T_tip=$(basename "$(dirname "$(ls -d "$TIP"/*/libstdc++-v3 | head -n1)")")
HDR_tip=(-nostdinc++
  -I"$TIP/$T_tip/libstdc++-v3/include/$T_tip"
  -I"$TIP/$T_tip/libstdc++-v3/include"
  -I"$SRC_tip/libstdc++-v3/libsupc++")
SRC_base=''
T_base=''
HDR_base=()
if [ -n "$BASE" ]; then
  test -x "$BASE/gcc/xg++" || { echo "::error::$BASE/gcc/xg++ missing"; exit 1; }
  SRC_base=$(cat "$BASE/.perf-lab-src")
  T_base=$(basename "$(dirname "$(ls -d "$BASE"/*/libstdc++-v3 | head -n1)")")
  HDR_base=(-nostdinc++
    -I"$BASE/$T_base/libstdc++-v3/include/$T_base"
    -I"$BASE/$T_base/libstdc++-v3/include"
    -I"$SRC_base/libstdc++-v3/libsupc++")
fi

# comp SIDE ARGS... : one compile, output passing through (for verify/strace)
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

# ---- stats helpers (rc=0 rows only) -----------------------------------------
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
# pct NEW OLD -> signed percent delta of NEW vs OLD
pct() { awk -v a="$1" -v b="$2" 'BEGIN{
  if (a=="nan" || b=="nan" || a=="n/a" || b=="n/a" || b+0==0) { print "n/a"; exit }
  printf "%+.1f%%", (a-b)/b*100 }'; }
# ratio OLD NEW -> OLD/NEW as multiplier
ratio() { awk -v a="$1" -v b="$2" 'BEGIN{
  if (a=="nan" || b=="nan" || b+0==0) { print "n/a"; exit }
  printf "%.2fx", a/b }'; }

# ---- environment header ------------------------------------------------------
env_header() {
  say "# perf-lab results -- suite=\`$SUITE\`"
  say ""
  say "- date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  say "- runner: \`${RUNNER_NAME:-?}\` on \`$(hostname 2>/dev/null || echo '?')\`, $(nproc) CPUs, $(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || echo '?') RAM"
  say "- cpu: $(lscpu 2>/dev/null | awk -F': +' '/Model name/{print $2; exit}' || echo unknown)"
  say "- THP: enabled=\`$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo n/a)\` defrag=\`$(cat /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || echo n/a)\`"
  say "- pin: ${PINCMD[*]:-none (taskset unavailable)}"
  say "- tip ($REFLABEL): \`$(cat "$TIP/.perf-lab-sha")\` -- $("$TIP/gcc/xg++" --version 2>/dev/null | head -n1)"
  if [ -n "$BASE" ]; then
    say "- base: \`$(cat "$BASE/.perf-lab-sha")\` -- $("$BASE/gcc/xg++" --version 2>/dev/null | head -n1)"
  fi
  say ""
}

# ---- realism -I args ----------------------------------------------------------
RARGS=()
for d in "$BENCH"/realism/roots/r*; do RARGS+=(-I"$d"); done

# =============================== suites ======================================

suite_quick() {
  say "## quick suite (tip only, -O2 wall + RSS + faults)"
  for w in 1 2; do
    ROUND=w$w
    timed tip warm-stl -O2 -c "$BENCH/bench_stl.cpp" -o /dev/null
    timed tip warm-big -O2 -I"$BENCH" -c "$BENCH/big_tu.cpp" -o /dev/null
  done
  for i in 1 2 3 4 5 6 7; do
    ROUND=$i
    timed tip stl-O2 -O2 -c "$BENCH/bench_stl.cpp" -o /dev/null
    timed tip big-O2 -O2 -I"$BENCH" -c "$BENCH/big_tu.cpp" -o /dev/null
    echo "quick round $i/7 done"
  done
  say ""
  say "| cell | wall median (s) | min..max | n | max RSS (MB) | minor faults |"
  say "|---|---|---|---|---|---|"
  local c
  for c in stl-O2 big-O2; do
    say "| $c | $(mwall "$c" tip) | $(wrange "$c" tip) | $(nok "$c" tip) | $(mrssmb "$c" tip) | $(mflt "$c" tip) |"
  done
}

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
  say ""
  say "THP counter delta around one huge_tu -O2 compile (**$sd**):"
  say '```'
  say "$delta"
  say '```'
}

suite_composed() {
  say "## composed suite (base vs tip, interleaved rounds)"
  say ""
  say "Cells: light-O0, stl-O0, stl-O2, big-O2, realism-O0 (median of 7);"
  say "huge-O2 and huge-O2-never (\`--param ggc-min-expand=2147483647\`, median of 5)."
  for w in 1 2; do
    ROUND=w$w
    local sd
    for sd in base tip; do
      timed $sd warm-stl -O2 -c "$BENCH/bench_stl.cpp" -o /dev/null
      timed $sd warm-big -O2 -I"$BENCH" -c "$BENCH/big_tu.cpp" -o /dev/null
    done
  done
  local i sd
  for i in 1 2 3 4 5 6 7; do
    ROUND=$i
    for sd in base tip; do
      timed $sd light-O0   -O0 -c "$BENCH/bench_light.cpp" -o /dev/null
      timed $sd stl-O0     -O0 -c "$BENCH/bench_stl.cpp" -o /dev/null
      timed $sd stl-O2     -O2 -c "$BENCH/bench_stl.cpp" -o /dev/null
      timed $sd big-O2     -O2 -I"$BENCH" -c "$BENCH/big_tu.cpp" -o /dev/null
      timed $sd realism-O0 "${RARGS[@]}" -O0 -c "$BENCH/realism/realism_tu.cpp" -o /dev/null
    done
    echo "composed round $i/7 done ($(uptime 2>/dev/null | sed 's/.*load/load/' || true))"
  done
  ROUND=wh
  for sd in base tip; do
    timed $sd warm-huge -O2 -I"$BENCH" -c "$BENCH/huge_tu.cpp" -o /dev/null
  done
  for i in 1 2 3 4 5; do
    ROUND=$i
    for sd in base tip; do
      timed $sd huge-O2 -O2 -I"$BENCH" -c "$BENCH/huge_tu.cpp" -o /dev/null
      timed $sd huge-O2-never -O2 -I"$BENCH" --param ggc-min-expand=2147483647 \
        -c "$BENCH/huge_tu.cpp" -o /dev/null
    done
    echo "huge round $i/5 done"
  done

  say ""
  say "### composed A/B (medians; base = pre-optimization, tip = $REFLABEL)"
  say ""
  say "| cell | base wall (s) | tip wall (s) | d wall | base minflt | tip minflt | d faults | base RSS MB | tip RSS MB |"
  say "|---|---|---|---|---|---|---|---|---|"
  local c wb wt fb ft
  for c in light-O0 stl-O0 stl-O2 big-O2 realism-O0 huge-O2 huge-O2-never; do
    wb=$(mwall "$c" base); wt=$(mwall "$c" tip)
    fb=$(mflt "$c" base);  ft=$(mflt "$c" tip)
    say "| $c | $wb | $wt | $(pct "$wt" "$wb") | $fb | $ft | $(pct "$ft" "$fb") | $(mrssmb "$c" base) | $(mrssmb "$c" tip) |"
  done

  # THP expression evidence (the sandbox could never show this)
  thp_probe base
  thp_probe tip

  # syscall profile on the realism TU (dir-index / readlink wins)
  if command -v strace >/dev/null 2>&1; then
    say ""
    say "### strace -c on realism-O0 (cc1plus via -wrapper)"
    say ""
    say "| side | total syscalls | openat | readlink |"
    say "|---|---|---|---|"
    for sd in base tip; do
      comp $sd "${RARGS[@]}" -wrapper strace,-f,-c,-o,"$OUT/realism_$sd.strace" \
        -O0 -c "$BENCH/realism/realism_tu.cpp" -o /dev/null || true
      if [ -f "$OUT/realism_$sd.strace" ]; then
        local tot op rl
        tot=$(awk '/^100\.00/{print $4; exit}' "$OUT/realism_$sd.strace")
        op=$(awk '$NF=="openat"{print $4; exit}' "$OUT/realism_$sd.strace")
        rl=$(awk '$NF=="readlink"{print $4; exit}' "$OUT/realism_$sd.strace")
        say "| $sd | ${tot:-?} | ${op:-0} | ${rl:-0} |"
      else
        say "| $sd | (strace produced no output) | | |"
      fi
    done
  else
    say ""
    say "(strace not present -- syscall table skipped)"
  fi
}

suite_pch() {
  say "## pch suite (tip only) -- PCH ceiling A/B"
  say ""
  say "Driver \`-x c++-header\` is broken in this fork (duplicate -o from the"
  say "integrated-as single-process change), so .gch files are built by driving"
  say "cc1plus directly; PCH *use* goes through the driver as normal."
  test -x "$TIP/gcc/cc1plus" || { echo "::error::$TIP/gcc/cc1plus missing"; exit 1; }

  local P=$BENCH/pch
  rm -rf "$P"
  local GCHTSV=$OUT/gch-builds.tsv
  printf 'prelude\tcell\tbuild_s\tgch_MB\trc\n' > "$GCHTSV"

  build_gch() { # PRELUDE CELL CFLAGS...
    local pre=$1 cell=$2; shift 2
    local d=$P/$cell
    mkdir -p "$d"
    cp "$BENCH/prelude_$pre.h" "$d/"
    local XI=()
    if [ "$pre" = json ]; then XI=(-I "$BENCH"); fi
    local t0 t1 rc=0 sz
    t0=$(date +%s.%N)
    "${PINCMD[@]}" "$TIP/gcc/cc1plus" -quiet -nostdinc++ \
      -I "$TIP/$T_tip/libstdc++-v3/include/$T_tip" \
      -I "$TIP/$T_tip/libstdc++-v3/include" \
      -I "$SRC_tip/libstdc++-v3/libsupc++" \
      "${XI[@]}" \
      -isystem "$TIP/gcc/include" -isystem "$TIP/gcc/include-fixed" \
      -D_GNU_SOURCE -imultiarch x86_64-linux-gnu \
      "$d/prelude_$pre.h" -dumpbase "prelude_$pre.h" -dumpbase-ext .h \
      -mtune=generic -march=x86-64 "$@" \
      -o /dev/null --output-pch "$d/prelude_$pre.h.gch" \
      2> "$d/prelude_$pre.$cell.err" || rc=$?
    t1=$(date +%s.%N)
    sz=$(stat -c %s "$d/prelude_$pre.h.gch" 2>/dev/null || echo 0)
    printf '%s\t%s\t%s\t%s\trc=%s\n' "$pre" "$cell" \
      "$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')" \
      "$(awk -v s="$sz" 'BEGIN{printf "%.1f", s/1048576}')" "$rc" >> "$GCHTSV"
    if [ "$rc" -ne 0 ]; then
      echo "::warning::gch build failed: $pre/$cell (rc=$rc)"
      head -20 "$d/prelude_$pre.$cell.err" || true
    fi
  }

  build_gch stl  O0  -O0
  build_gch stl  O0g -O0 -g
  build_gch stl  O2  -O2
  build_gch json O0  -O0
  build_gch json O2  -O2

  say ""
  say "### .gch inventory (direct-cc1plus builds)"
  say ""
  say "| prelude | cell | build (s) | size (MB) | rc |"
  say "|---|---|---|---|---|"
  awk -F'\t' 'NR>1{printf "| %s | %s | %s | %s | %s |\n", $1,$2,$3,$4,$5}' "$GCHTSV" \
    | while IFS= read -r line; do say "$line"; done

  # verification: -H '!' marker + .o byte identity
  local VER=$OUT/pch-verify.tsv
  printf 'label\thbang\tidentical\n' > "$VER"
  verify_pch() { # TU LABEL PRELUDE CELL CFLAGS...
    local tu=$1 label=$2 pre=$3 cell=$4; shift 4
    local inc=()
    case "$tu" in *big_tu*|*bench_json*) inc=(-I"$BENCH") ;; esac
    local hn=0 ident=SKIP
    comp tip "${inc[@]}" "$@" -H -Winvalid-pch -include "$P/$cell/prelude_$pre.h" \
      -fsyntax-only "$tu" 2> "$OUT/H_$label.txt" || true
    hn=$(grep -c '^!' "$OUT/H_$label.txt" 2>/dev/null || true)
    if comp tip "${inc[@]}" "$@" -c "$tu" -o "$OUT/id.nopch.o" \
       && comp tip "${inc[@]}" "$@" -Winvalid-pch -include "$P/$cell/prelude_$pre.h" \
            -c "$tu" -o "$OUT/id.pch.o"; then
      if cmp -s "$OUT/id.nopch.o" "$OUT/id.pch.o"; then ident=yes; else ident=NO; fi
    else
      ident=compile-failed
    fi
    rm -f "$OUT/id.nopch.o" "$OUT/id.pch.o"
    printf '%s\t%s\t%s\n' "$label" "$hn" "$ident" >> "$VER"
  }

  verify_pch "$BENCH/bench_stl.cpp"  stl-O0   stl  O0  -O0
  verify_pch "$BENCH/bench_stl.cpp"  stl-O0g  stl  O0g -O0 -g
  verify_pch "$BENCH/bench_stl.cpp"  stl-O2   stl  O2  -O2
  verify_pch "$BENCH/bench_json.cpp" json-O0  json O0  -O0
  verify_pch "$BENCH/bench_json.cpp" json-O2  json O2  -O2
  verify_pch "$BENCH/big_tu.cpp"     big-O2   stl  O2  -O2

  pch_pair() { # TU LABEL PRELUDE CELL NRUNS CFLAGS...
    local tu=$1 lab=$2 pre=$3 cell=$4 n=$5; shift 5
    local inc=()
    case "$tu" in *big_tu*|*bench_json*) inc=(-I"$BENCH") ;; esac
    local w i
    for w in 1 2; do
      ROUND=w$w
      timed tip "warm-$lab" "${inc[@]}" "$@" -c "$tu" -o /dev/null
      timed tip "warm-$lab-pch" "${inc[@]}" "$@" -Winvalid-pch \
        -include "$P/$cell/prelude_$pre.h" -c "$tu" -o /dev/null
    done
    for i in $(seq 1 "$n"); do
      ROUND=$i
      timed tip "nopch-$lab" "${inc[@]}" "$@" -c "$tu" -o /dev/null
      timed tip "pch-$lab" "${inc[@]}" "$@" -Winvalid-pch \
        -include "$P/$cell/prelude_$pre.h" -c "$tu" -o /dev/null
    done
    echo "pch pair $lab done"
  }

  pch_pair "$BENCH/bench_stl.cpp"  stl-O0    stl  O0  7 -O0
  pch_pair "$BENCH/bench_stl.cpp"  stl-O0g   stl  O0g 7 -O0 -g
  pch_pair "$BENCH/bench_stl.cpp"  stl-O2    stl  O2  7 -O2
  pch_pair "$BENCH/bench_json.cpp" json-O0   json O0  7 -O0
  pch_pair "$BENCH/bench_json.cpp" json-O2   json O2  7 -O2
  pch_pair "$BENCH/big_tu.cpp"     big-O2    stl  O2  7 -O2
  pch_pair "$BENCH/anchor.cpp"     anchor-O0 stl  O0  7 -O0

  say ""
  say "### PCH ceiling A/B (tip, medians; anchor-O0 = pure PCH load floor)"
  say ""
  say "| TU/cell | no PCH (s) | PCH (s) | speedup | d wall | -H '!' lines | .o identical |"
  say "|---|---|---|---|---|---|---|"
  local lab a b hb id
  for lab in stl-O0 stl-O0g stl-O2 json-O0 json-O2 big-O2 anchor-O0; do
    a=$(mwall "nopch-$lab" tip)
    b=$(mwall "pch-$lab" tip)
    hb=$(awk -F'\t' -v l="$lab" '$1==l{print $2}' "$VER")
    id=$(awk -F'\t' -v l="$lab" '$1==l{print $3}' "$VER")
    say "| $lab | $a | $b | $(ratio "$a" "$b") | $(pct "$b" "$a") | ${hb:-n/a} | ${id:-n/a} |"
  done
  say ""
  say "(anchor-O0 has no identity/-H row: with -include the token stream differs by design.)"
}

# ================================ main =======================================
env_header
case $SUITE in
  quick)    suite_quick ;;
  composed) suite_composed ;;
  pch)      suite_pch ;;
  full)     suite_composed; suite_pch ;;
esac

say ""
say "Raw per-run TSV (\`perf-lab.tsv\`), \`/usr/bin/time -v\` outputs (\`tv.d/\`),"
say "strace/THP/PCH verification files are in the \`perf-lab-results\` artifact."
echo "SUITE $SUITE COMPLETE"
