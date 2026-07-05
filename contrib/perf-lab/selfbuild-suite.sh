#!/bin/bash
# perf-lab: selfbuild suite -- time a stage's compiler building a PINNED
# GCC-source workload ("workload v1").
#
#   selfbuild-suite.sh STAGE_SRC WORKLOAD_SRC [OUTDIR] [DLCACHE]
#
# Feeds the chronological table in the repo-root README.md ("Compile-time
# performance work"): one run measures ONE stage of the fork and emits a
# ready-to-paste README row. The measured question is always the same:
# "how long does THIS stage's compiler take to compile the SAME pinned set
# of GCC source files?" -- so rows are comparable down the table.
#
# Phases:
#   1. Build the stage compiler from STAGE_SRC with the recipe class the
#      tree itself demands (auto-detected): contrib/gas-embed present ->
#      the exact ci.yml combined-binutils-tree recipe via build-gcc.sh
#      (every fork stage from PR #2 on); absent -> plain upstream recipe,
#      same configure flags, no binutils embed (the "stock" fork point).
#      The build wall doubles as the secondary "system g++ builds the
#      stage source" datum (source-size-confounded: later stages carry
#      more code -- context, not the headline).
#   2. Prepare the workload tree: configure WORKLOAD_SRC and `make all-gcc`
#      with the SYSTEM compiler, purely to materialize the generated
#      headers (config.h, options.h, insn-*.h, gt-*.h, ...) the workload
#      files #include. The workload checkout is pinned (see perf-lab.yml),
#      so this build's wall is a constant-input runner-speed calibration
#      datum on every run.
#   3. Measure: compile every file in selfbuild-workload-v1.txt with the
#      STAGE compiler using the pinned argv (extracted verbatim from a
#      real build tree's make output; see PINNED ARGV below):
#        - one -j$JOBS warmup pass (page cache + binaries; not reported),
#        - PASSES (default 3) serial passes, each file pinned to one CPU
#          under /usr/bin/time; per-pass metric = SUM of per-file walls;
#          headline = MEDIAN of the pass sums,
#        - one -j$JOBS wall-clock pass (the "how long does a parallel
#          build feel" number; single-shot, noisier than the serial sum).
#      Compiler caches are OFF: no -fcompile-cache, no -fauto-pch, cache/
#      auto-PCH env stripped -- rows measure the compiler's COLD path.
#      Page cache is deliberately WARM after the warmup pass.
#   4. Emit results/selfbuild-summary.tsv, results/selfbuild.tsv (raw
#      per-compile rows), results/README-row.md (paste into the root
#      README table) and a step-summary section.
#
# Any compile failure in any pass is fatal (exit 1): a partial sum is not
# a comparable number.
#
# Env knobs:
#   STAGE_LABEL              row label (default: stage short SHA)
#   PIN_CPU                  serial-pass pin target (default 2, run-shard's)
#   SELFBUILD_PASSES         timed serial passes (default 3; protocol value)
#   SELFBUILD_JOBS           warmup/-jN parallelism (default: nproc)
#   SELFBUILD_WORKLOAD_LIST  workload list file (default: v1 next to script)
#   Dev-only (numbers taken with these are NOT protocol numbers):
#   SELFBUILD_SUBSET=N            only the first N workload files
#   SELFBUILD_REUSE_STAGE_BUILD   existing build tree, skip phase 1
#   SELFBUILD_REUSE_WORKLOAD_BUILD existing build tree, skip phase 2
set -euo pipefail

STAGE_SRC=${1:?usage: selfbuild-suite.sh STAGE_SRC WORKLOAD_SRC [OUTDIR] [DLCACHE]}
WORKLOAD_SRC=${2:?usage: selfbuild-suite.sh STAGE_SRC WORKLOAD_SRC [OUTDIR] [DLCACHE]}
OUT=${3:-$PWD/results}
DL=${4:-$PWD/dl}

RIGDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
LIST=${SELFBUILD_WORKLOAD_LIST:-$RIGDIR/selfbuild-workload-v1.txt}
PIN=${PIN_CPU:-2}
PASSES=${SELFBUILD_PASSES:-3}
JOBS=${SELFBUILD_JOBS:-$(nproc)}
WORK=${SELFBUILD_WORK:-$PWD/selfbuild-work}

test -f "$STAGE_SRC/configure"    || { echo "::error::$STAGE_SRC is not a GCC source tree"; exit 1; }
test -f "$WORKLOAD_SRC/configure" || { echo "::error::$WORKLOAD_SRC is not a GCC source tree"; exit 1; }
test -f "$LIST" || { echo "::error::workload list missing: $LIST"; exit 1; }
command -v /usr/bin/time >/dev/null 2>&1 || {
  echo "::error::/usr/bin/time missing (apt package 'time')"; exit 1; }

mkdir -p "$OUT" "$WORK"
SUMMARY=${GITHUB_STEP_SUMMARY:-$OUT/summary.md}
say() {
  printf '%s\n' "$*"
  printf '%s\n' "$*" >> "$SUMMARY"
  if [ "$SUMMARY" != "$OUT/summary.md" ]; then
    printf '%s\n' "$*" >> "$OUT/summary.md"
  fi
}
now() { date +%s; }
secs() { case $1 in ''|*[!0-9]*) printf '%s' "$1" ;; *) printf '%ss' "$1" ;; esac; }
med() { sort -g | awk '{a[NR]=$1} END{
  if (!NR) { print "nan"; exit }
  if (NR%2) printf "%.2f\n", a[(NR+1)/2];
  else printf "%.2f\n", (a[NR/2]+a[NR/2+1])/2 }'; }

# The compiler under test must never see a configured compile cache or
# auto-PCH: rows measure the cold path. (A hosted runner env is clean;
# this is defense against local/dev use.)
unset GCC_COMPILE_CACHE_DIR GCC_COMPILE_CACHE_DEBUG GCC_COMPILE_CACHE_LINK \
      GCC_COMPILE_CACHE_VERIFY GCC_COMPILE_CACHE_SALT \
      GCC_AUTO_PCH GCC_AUTO_PCH_DEBUG \
      GCC_EXEC_PREFIX COMPILER_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH \
      DEPENDENCIES_OUTPUT SOURCE_DATE_EPOCH || true
export LC_ALL=C

recipe_of() {
  if [ -f "$1/contrib/gas-embed/gas-integrated-as.patch" ]; then
    echo combined
  else
    echo plain
  fi
}

# plain upstream recipe: same configure flags as build-gcc.sh / ci.yml,
# minus every combined-tree/libgas step. Used for the pre-fork "stock"
# stage (and any tree without contrib/gas-embed). MODE=full also builds
# the in-tree libstdc++ (needed for the stage compiler's header set);
# MODE=allgcc stops at all-gcc (workload prep needs only generated files).
build_plain() {
  local src=$1 build=$2 mode=$3 j
  j=$(nproc)
  rm -rf "$build"
  mkdir -p "$build"
  (
    cd "$build"
    "$src/configure" \
      --disable-bootstrap --enable-languages=c,c++ --disable-multilib \
      --with-system-zlib --disable-nls --disable-werror \
      MAKEINFO=true
    make -j"$j" all-gcc MAKEINFO=true
    if [ "$mode" = full ]; then
      make -j"$j" all-target-libstdc++-v3 MAKEINFO=true
    fi
  )
  printf '%s\n' "$src" > "$build/.perf-lab-src"
  (git -C "$src" rev-parse HEAD 2>/dev/null || echo unknown) > "$build/.perf-lab-sha"
}

smoke_stage() { # BUILD SRC -- same <vector> smoke test as build-gcc.sh
  local build=$1 src=$2 t
  t=$(basename "$(dirname "$(dirname "$(ls -d "$build"/*/libstdc++-v3/include | head -n1)")")")
  printf '#include <vector>\nint f(){ std::vector<int> v{1,2,3}; return (int)v.size(); }\n' \
    > "$build/.selfbuild-smoke.cpp"
  "$build/gcc/xg++" -B"$build/gcc" -nostdinc++ \
    -I"$build/$t/libstdc++-v3/include/$t" \
    -I"$build/$t/libstdc++-v3/include" \
    -I"$src/libstdc++-v3/libsupc++" \
    -O2 -c "$build/.selfbuild-smoke.cpp" -o "$build/.selfbuild-smoke.o"
}

# ---- phase 1: build the stage compiler --------------------------------------
STAGE_RECIPE=$(recipe_of "$STAGE_SRC")
STAGE_BUILD=$WORK/stage-build
STAGE_BUILD_WALL=reused
if [ -n "${SELFBUILD_REUSE_STAGE_BUILD:-}" ]; then
  STAGE_BUILD=$SELFBUILD_REUSE_STAGE_BUILD
  echo "phase 1: REUSING stage build $STAGE_BUILD (dev mode)"
else
  echo "phase 1: building stage compiler ($STAGE_RECIPE recipe) from $STAGE_SRC"
  t0=$(now)
  if [ "$STAGE_RECIPE" = combined ]; then
    bash "$RIGDIR/build-gcc.sh" "$STAGE_SRC" "$STAGE_BUILD" "$DL"
  else
    build_plain "$STAGE_SRC" "$STAGE_BUILD" full
    smoke_stage "$STAGE_BUILD" "$STAGE_SRC"
  fi
  STAGE_BUILD_WALL=$(( $(now) - t0 ))
  echo "phase 1 done in ${STAGE_BUILD_WALL}s"
fi
test -x "$STAGE_BUILD/gcc/xg++" || { echo "::error::stage build has no gcc/xg++"; exit 1; }
STAGE_SHA=$(cat "$STAGE_BUILD/.perf-lab-sha" 2>/dev/null || echo unknown)

# ---- phase 2: prepare the pinned workload tree -------------------------------
WORKLOAD_RECIPE=$(recipe_of "$WORKLOAD_SRC")
WB=$WORK/workload-build
PREP_WALL=reused
if [ -n "${SELFBUILD_REUSE_WORKLOAD_BUILD:-}" ]; then
  WB=$SELFBUILD_REUSE_WORKLOAD_BUILD
  echo "phase 2: REUSING workload build $WB (dev mode)"
else
  echo "phase 2: preparing workload tree ($WORKLOAD_RECIPE recipe) from $WORKLOAD_SRC"
  t0=$(now)
  if [ "$WORKLOAD_RECIPE" = combined ]; then
    # future-proofing: a re-pinned workload inside the fork range needs the
    # combined tree for its all-gcc (cc1 links libgas from PR #2 on)
    bash "$RIGDIR/build-gcc.sh" "$WORKLOAD_SRC" "$WB" "$DL"
  else
    build_plain "$WORKLOAD_SRC" "$WB" allgcc
  fi
  PREP_WALL=$(( $(now) - t0 ))
  echo "phase 2 done in ${PREP_WALL}s"
fi
test -f "$WB/gcc/config.h" || { echo "::error::workload build has no gcc/config.h (all-gcc failed?)"; exit 1; }
WORKLOAD_SHA=$(git -C "$WORKLOAD_SRC" rev-parse HEAD 2>/dev/null \
  || cat "$WB/.perf-lab-sha" 2>/dev/null || echo unknown)

# ---- phase 3: measurement -----------------------------------------------------
# Stage compiler front (same shape as run-shard.sh / pack-compiler.sh):
T=$(basename "$(dirname "$(dirname "$(ls -d "$STAGE_BUILD"/*/libstdc++-v3/include | head -n1)")")")
XG=$STAGE_BUILD/gcc/xg++
STAGE_FRONT=(-B"$STAGE_BUILD/gcc" -nostdinc++
  -I"$STAGE_BUILD/$T/libstdc++-v3/include/$T"
  -I"$STAGE_BUILD/$T/libstdc++-v3/include"
  -I"$STAGE_SRC/libstdc++-v3/libsupc++")

WS=$WORKLOAD_SRC

# PINNED ARGV (workload v1). Extracted verbatim from a real out-of-tree
# build of this fork (make -n output for tree.o, cp/parser.o, and libcpp
# charset.o), with only these edits: relative -I paths made absolute
# ($WB = workload build tree, $WS = workload source tree), duplicate -I
# entries collapsed, dependency-tracking flags (-MT/-MMD/-MP/-MF) dropped,
# and -g -O2 pinned explicitly (the toolchain default the ci.yml recipe
# builds with). DO NOT tweak these between rows: the argv is part of the
# workload definition.
GCC_ARGS=(-fno-PIE -c -g -O2 -DIN_GCC
  -fno-exceptions -fno-rtti -fasynchronous-unwind-tables
  -W -Wall -Wno-narrowing -Wwrite-strings -Wcast-qual
  -Wmissing-format-attribute -Wconditionally-supported -Woverloaded-virtual
  -pedantic -Wno-long-long -Wno-variadic-macros -Wno-overlength-strings
  -DHAVE_CONFIG_H -fno-PIE
  -I"$WB/gcc" -I"$WS/gcc" -I"$WS/include" -I"$WS/libcpp/include"
  -I"$WS/libcody" -I"$WS/libdecnumber" -I"$WS/libdecnumber/bid"
  -I"$WB/libdecnumber" -I"$WS/libbacktrace")
CP_ARGS=(-fno-PIE -c -DIN_GCC_FRONTEND -g -O2 -DIN_GCC
  -fno-exceptions -fno-rtti -fasynchronous-unwind-tables
  -W -Wall -Wno-narrowing -Wwrite-strings -Wcast-qual
  -Wmissing-format-attribute -Wconditionally-supported -Woverloaded-virtual
  -pedantic -Wno-long-long -Wno-variadic-macros -Wno-overlength-strings
  -DHAVE_CONFIG_H -fno-PIE
  -I"$WB/gcc" -I"$WB/gcc/cp" -I"$WS/gcc" -I"$WS/gcc/cp"
  -I"$WS/include" -I"$WS/libcpp/include"
  -I"$WS/libcody" -I"$WS/libdecnumber" -I"$WS/libdecnumber/bid"
  -I"$WB/libdecnumber" -I"$WS/libbacktrace")
LIBCPP_ARGS=(-I"$WS/libcpp" -I"$WB/libcpp" -I"$WS/include" -I"$WS/libcpp/include"
  -g -O2
  -W -Wall -Wno-narrowing -Wwrite-strings -Wmissing-format-attribute
  -pedantic -Wno-long-long -fno-exceptions -fno-rtti -c)
# cp/module.o is the ONE workload file with per-file defines
# (cp/Make-lang.in: CFLAGS-cp/module.o += -DHOST_MACHINE/-DTARGET_MACHINE;
# its MODULE_VERSION define is DEVPHASE-gated and absent on this release
# branch -- verified against the real build's make -n output). Values are
# pinned literals on purpose: the suite targets x86_64 Linux runners and
# the workload argv must never vary run-to-run. A sweep of the other 40
# files' real compile lines found no other per-file flags. Missing these
# two defines failed cp/module.cc on every stage in the first anchor
# dispatches (runs 28727557771 / 28727562357 / 28727587950).
MODULE_EXTRA=(-DHOST_MACHINE='"x86_64-pc-linux-gnu"'
  -DTARGET_MACHINE='"x86_64-pc-linux-gnu"')

# Workload file list -> one generated command script per file. The same
# scripts run in every pass (serial and -jN), so the compile argv is
# byte-identical across modes.
CMDD=$WORK/cmds ERRD=$WORK/err OBJD=$WORK/obj FAILLOG=$WORK/failed.lst
rm -rf "$CMDD" "$ERRD" "$OBJD"
mkdir -p "$CMDD" "$ERRD" "$OBJD"
: > "$FAILLOG"

FILES=()
while IFS= read -r line; do
  case $line in ''|'#'*) continue ;; esac
  FILES+=("$line")
done < "$LIST"
if [ -n "${SELFBUILD_SUBSET:-}" ]; then
  FILES=("${FILES[@]:0:$SELFBUILD_SUBSET}")
  echo "::warning::SELFBUILD_SUBSET=$SELFBUILD_SUBSET -- NOT protocol numbers"
fi
NFILES=${#FILES[@]}
[ "$NFILES" -gt 0 ] || { echo "::error::empty workload list"; exit 1; }

i=0
for f in "${FILES[@]}"; do
  src=$WS/$f
  test -f "$src" || { echo "::error::workload file missing: $src"; exit 1; }
  case $f in
    libcpp/*)          args=("${LIBCPP_ARGS[@]}") ;;
    gcc/cp/module.cc)  args=("${MODULE_EXTRA[@]}" "${CP_ARGS[@]}") ;;
    gcc/cp/*)          args=("${CP_ARGS[@]}") ;;
    gcc/*)             args=("${GCC_ARGS[@]}") ;;
    *) echo "::error::unclassifiable workload path: $f"; exit 1 ;;
  esac
  n=$(printf '%03d' "$i")
  {
    printf '#!/bin/bash\nexec '
    printf '%q ' "$XG" "${STAGE_FRONT[@]}" "${args[@]}" \
      -o "$OBJD/$n.o" "$src"
    printf '> /dev/null 2> %q\n' "$ERRD/$n.err"
  } > "$CMDD/$n.sh"
  i=$((i + 1))
done

# one preflight compile so a broken setup fails in seconds, not after a pass
bash "$CMDD/000.sh" || {
  echo "::error::preflight compile failed: ${FILES[0]} (stderr follows)"
  cat "$ERRD/000.err" || true
  exit 1
}
echo "preflight OK: ${FILES[0]} compiled by the stage compiler"

PINCMD=()
if command -v taskset >/dev/null 2>&1; then
  NC=$(nproc)
  if [ "$PIN" -ge "$NC" ]; then PIN=$((NC - 1)); fi
  if taskset -c "$PIN" true 2>/dev/null; then PINCMD=(taskset -c "$PIN"); fi
fi
[ ${#PINCMD[@]} -gt 0 ] || echo "::warning::taskset unavailable; serial passes unpinned"

TSV=$OUT/selfbuild.tsv
printf 'pass\tidx\tfile\twall_s\tmaxrss_kb\tuser_s\tsys_s\trc\n' > "$TSV"

run_one_timed() { # PASS IDX -> appends TSV row; returns compile rc
  local pass=$1 idx=$2 rc=0 tf line
  tf=$WORK/t.tmp
  /usr/bin/time -f '%e %M %U %S' -o "$tf" \
    "${PINCMD[@]}" bash "$CMDD/$idx.sh" || rc=$?
  line=$(tail -n1 "$tf" 2>/dev/null || echo 'nan 0 nan nan')
  # intentional word split of time(1)'s '%e %M %U %S' output
  # shellcheck disable=SC2086
  set -- $line
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$pass" "$idx" "${FILES[10#$idx]}" "${1:-nan}" "${2:-0}" "${3:-nan}" "${4:-nan}" "$rc" >> "$TSV"
  return "$rc"
}

parallel_pass() { # PASS -> wall seconds on stdout; fails if any compile failed
  local pass=$1 t0 t1 rc=0
  : > "$FAILLOG"
  t0=$(now)
  printf '%s\n' "$CMDD"/*.sh | \
    FAILLOG=$FAILLOG xargs -P "$JOBS" -I{} \
      bash -c 'bash "$1" || { echo "$1" >> "$FAILLOG"; exit 1; }' _ {} || rc=$?
  t1=$(now)
  if [ -s "$FAILLOG" ]; then
    echo "::error::$pass pass: $(wc -l < "$FAILLOG") compile(s) failed:" >&2
    while IFS= read -r failed; do
      fn=$(basename "$failed" .sh)
      echo "--- ${FILES[10#$fn]} (idx $fn) stderr head:" >&2
      head -n 15 "$ERRD/$fn.err" >&2 || true
    done < "$FAILLOG"
    return 1
  fi
  [ "$rc" -eq 0 ] || { echo "::error::$pass pass: xargs rc=$rc with empty fail log" >&2; return 1; }
  echo $((t1 - t0))
}

echo "workload: $NFILES files x (1 warmup + $PASSES serial + 1 -j$JOBS) passes"

# warmup (parallel: warms page cache and compiler binaries; not reported)
WARM_WALL=$(parallel_pass warmup) || exit 1
echo "warmup (-j$JOBS) done in ${WARM_WALL}s"

# timed serial passes, pinned
declare -a PASS_SUMS=()
p=1
while [ "$p" -le "$PASSES" ]; do
  t0=$(now)
  FAILED=0
  i=0
  while [ "$i" -lt "$NFILES" ]; do
    n=$(printf '%03d' "$i")
    run_one_timed "s$p" "$n" || FAILED=1
    i=$((i + 1))
  done
  t1=$(now)
  if [ "$FAILED" -ne 0 ]; then
    echo "::error::serial pass s$p had compile failures (see $TSV):"
    awk -F'\t' -v pp="s$p" '$1==pp && $8!=0 {print "  " $3 " rc=" $8}' "$TSV"
    awk -F'\t' -v pp="s$p" '$1==pp && $8!=0 {print $2}' "$TSV" | while IFS= read -r fn; do
      echo "--- idx $fn stderr head:"; head -n 15 "$ERRD/$fn.err" || true
    done
    exit 1
  fi
  sum=$(awk -F'\t' -v pp="s$p" '$1==pp {s+=$4} END{printf "%.2f", s}' "$TSV")
  PASS_SUMS+=("$sum")
  echo "serial pass s$p: sum ${sum}s (bracket wall $((t1 - t0))s)"
  p=$((p + 1))
done
SERIAL_MED=$(printf '%s\n' "${PASS_SUMS[@]}" | med)

# timed -jN pass (fully warm)
J4_WALL=$(parallel_pass "j$JOBS") || exit 1
echo "-j$JOBS pass done in ${J4_WALL}s"

# ---- phase 4: emit ------------------------------------------------------------
STAGE_LABEL=${STAGE_LABEL:-$(printf '%.12s' "$STAGE_SHA")}
SHORT_SHA=$(printf '%.12s' "$STAGE_SHA")
if [ -n "${GITHUB_RUN_ID:-}" ]; then
  RUN_CELL="[${GITHUB_RUN_ID}](${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-wow-look-at-my/gcc}/actions/runs/${GITHUB_RUN_ID})"
else
  RUN_CELL=local
fi
HOSTLINE="$(nproc) vCPU, $(lscpu 2>/dev/null | sed -n 's/^Model name: *//p' | head -n1 || echo unknown), THP $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo n/a)"

SUMTSV=$OUT/selfbuild-summary.tsv
{
  printf 'stage_label\tstage_sha\tstage_recipe\tworkload_sha\tnfiles\tpasses\tserial_sums_s\tserial_median_s\tj%s_wall_s\twarmup_wall_s\tstage_build_wall_s\tworkload_prep_wall_s\thost\n' "$JOBS"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$STAGE_LABEL" "$STAGE_SHA" "$STAGE_RECIPE" "$WORKLOAD_SHA" "$NFILES" "$PASSES" \
    "$(IFS=,; echo "${PASS_SUMS[*]}")" "$SERIAL_MED" "$J4_WALL" "$WARM_WALL" \
    "$STAGE_BUILD_WALL" "$PREP_WALL" "$HOSTLINE"
} > "$SUMTSV"

# per-file medians across the serial passes (slow-file forensics)
awk -F'\t' '$1 ~ /^s[0-9]+$/ && $8==0 { k=$2 "\t" $3; v[k]=v[k] $4 " " }
  END { for (k in v) { n=split(v[k], a, " ") - 1; asort_n(a, n, k) } }
  function asort_n(a, n, k,   i, j, t) {
    for (i=1; i<=n; i++) for (j=i+1; j<=n; j++) if (a[j]+0 < a[i]+0) { t=a[i]; a[i]=a[j]; a[j]=t }
    m = (n % 2) ? a[(n+1)/2] : (a[n/2] + a[n/2+1]) / 2
    printf "%s\t%.2f\n", k, m
  }' "$TSV" | sort > "$OUT/selfbuild-files.tsv"

ROW="| $STAGE_LABEL | ? | \`$SHORT_SHA\` | $SERIAL_MED s serial-sum / $J4_WALL s -j$JOBS | ? | $RUN_CELL | stage build $(secs "$STAGE_BUILD_WALL"), workload prep $(secs "$PREP_WALL") (system g++) |"
printf '%s\n' "$ROW" > "$OUT/README-row.md"

say ""
say "## selfbuild: $STAGE_LABEL"
say ""
say "- stage: \`$STAGE_SHA\` ($STAGE_RECIPE recipe)"
say "- workload: v1, $NFILES files @ \`$WORKLOAD_SHA\`"
say "- host: $HOSTLINE"
say "- **serial -j1 pinned sum: median $SERIAL_MED s** (passes: $(IFS=,; echo "${PASS_SUMS[*]}"))"
say "- **-j$JOBS wall: $J4_WALL s** (single-shot)"
say "- stage compiler build (system g++): $(secs "$STAGE_BUILD_WALL") -- source-size-confounded, context only"
say "- workload prep all-gcc (system g++, pinned input): $(secs "$PREP_WALL") -- runner calibration"
say ""
say "Ready-to-paste README row (fill Landed-in / delta / Notes by hand):"
say ""
say '```'
say "$ROW"
say '```'

echo "selfbuild suite done: $SUMTSV"
