#!/usr/bin/env bash
# CORRECTED compile-time benchmark (v2): fork gcc (PGO) vs system gcc building llama.cpp.
# Fix vs run_headline.sh: llama.cpp's GGML_CCACHE defaults ON -> ggml set a global
# RULE_LAUNCH_COMPILE=ccache and silently cached everything to /root/.cache/ccache,
# poisoning all "nocache"/warm numbers. EVERY config now passes -DGGML_CCACHE=OFF and
# per-run guards assert no ccache in the build (or, for ccache configs, that it IS there).
# Appends one JSON line per completed run to results2.jsonl. Resumable: runs already
# present in results2.jsonl are skipped (append-only).
set -euo pipefail

BENCH=/tmp/claude-0/-home-user/b5431e56-3b74-5bd3-89b4-e7d9cbd67820/scratchpad/bench
LLAMA=/home/user/llama.cpp
RESULTS=$BENCH/results2.jsonl
NPROC=$(nproc)
NINJA_JOBS=$((NPROC + 2))   # ninja default -j = nproc+2
FORK_CC=$BENCH/bin/fork-pgo-cc
FORK_CXX=$BENCH/bin/fork-pgo-cxx
GCC_CACHE2=$BENCH/gcc-cache2
CCACHE_FORK=$BENCH/ccache-fork

mkdir -p "$BENCH/logs" "$BENCH/ninjalogs" "$BENCH/objhash"
touch "$RESULTS"

PREV_BUILD=""
CUR_BUILD=""

log() { echo "[$(date -Is)] $*"; }
guard_fail() { log "GUARD FAIL: $*"; exit 42; }

has_result() { # config n
  grep -q "\"config\":\"$1\",\"run\":$2," "$RESULTS"
}

wait_idle() { # echoes a note ("" or timeout note); waits for loadavg < 0.6, max 180s
  local waited=0 la ok
  while :; do
    la=$(cut -d' ' -f1 /proc/loadavg)
    ok=$(awk -v l="$la" 'BEGIN{print (l<0.6)?1:0}')
    [ "$ok" = 1 ] && { echo ""; return 0; }
    if [ "$waited" -ge 180 ]; then echo "loadavg_wait_timeout(la=$la)"; return 0; fi
    sleep 10; waited=$((waited+10))
  done
}

elapsed_to_s() { # "h:mm:ss" or "m:ss.ff" -> seconds
  awk -v t="$1" 'BEGIN{n=split(t,a,":"); if(n==3){printf "%.2f", a[1]*3600+a[2]*60+a[3]} else if(n==2){printf "%.2f", a[1]*60+a[2]} else {printf "%.2f", a[1]}}'
}

run_build() { # config n expect_ccache(0|1) [extra cmake args...]
  local config="$1" n="$2" expect_ccache="$3"; shift 3
  local bdir="$BENCH/build2-$config-$n"
  local log_f="$BENCH/logs/$config-$n.log"
  local cfglog="$BENCH/logs/$config-$n.configure.log"

  if has_result "$config" "$n"; then
    log "SKIP $config run $n (already in results2.jsonl)"
    return 0
  fi

  log "=== $config run $n : configure ==="
  if [ -n "$PREV_BUILD" ] && [ -d "$PREV_BUILD" ]; then rm -rf "$PREV_BUILD"; fi
  rm -rf "$bdir"
  cmake -S "$LLAMA" -B "$bdir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF -DGGML_CCACHE=OFF "$@" > "$cfglog" 2>&1

  # ---- Poisoning guards (run BEFORE the timed build) ----
  if grep -q "compilation results will be cached" "$cfglog"; then
    guard_fail "$config-$n: configure log shows ggml enabled ccache despite GGML_CCACHE=OFF"
  fi
  local ccnt fcnt
  ccnt=$(ninja -C "$bdir" -t commands | grep -c 'ccache' || true)
  fcnt=$(ninja -C "$bdir" -t commands | grep -c 'fcompile-cache' || true)
  if [ "$expect_ccache" = 0 ] && [ "$ccnt" -ne 0 ]; then
    guard_fail "$config-$n: $ccnt ccache references in ninja commands (expected 0)"
  fi
  if [ "$expect_ccache" = 1 ] && [ "$ccnt" -eq 0 ]; then
    guard_fail "$config-$n: no ccache references in ninja commands (expected >0)"
  fi
  case "$config" in
    fork-cache-*) if [ "$fcnt" -eq 0 ]; then guard_fail "$config-$n: -fcompile-cache missing from commands"; fi ;;
    *)            if [ "$fcnt" -ne 0 ]; then guard_fail "$config-$n: unexpected -fcompile-cache in commands"; fi ;;
  esac
  log "guards ok for $config-$n: ccache_cmds=$ccnt fcompile_cache_cmds=$fcnt (expect_ccache=$expect_ccache)"

  local note; note=$(wait_idle)
  local la_before; la_before=$(cut -d' ' -f1 /proc/loadavg)
  local started; started=$(date -Is)

  log "=== $config run $n : ninja (timed) ==="
  local rc=0
  /usr/bin/time -v ninja -C "$bdir" > "$log_f" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    log "BUILD FAILED $config run $n rc=$rc — see $log_f"
    exit "$rc"
  fi

  cp "$bdir/.ninja_log" "$BENCH/ninjalogs/$config-$n.ninja_log"

  local wall user sys maxrss bsize
  wall=$(elapsed_to_s "$(grep 'Elapsed (wall clock) time' "$log_f" | awk '{print $NF}')")
  user=$(grep 'User time (seconds)' "$log_f" | awk '{print $NF}')
  sys=$(grep 'System time (seconds)' "$log_f" | awk '{print $NF}')
  maxrss=$(grep 'Maximum resident set size' "$log_f" | awk '{print $NF}')
  bsize=$(du -sh "$bdir" | cut -f1)

  printf '{"config":"%s","run":%s,"wall_s":%s,"user_s":%s,"sys_s":%s,"maxrss_kb":%s,"ninja_default_jobs":%s,"started_at":"%s","loadavg_before":%s,"build_dir_size":"%s","notes":"%s"}\n' \
    "$config" "$n" "$wall" "$user" "$sys" "$maxrss" "$NINJA_JOBS" "$started" "$la_before" "$bsize" "$note" >> "$RESULTS"
  log "DONE $config run $n wall=${wall}s user=${user}s sys=${sys}s maxrss=${maxrss}kB size=$bsize note='$note'"

  PREV_BUILD="$bdir"
  CUR_BUILD="$bdir"
}

objhash() { # config n  (uses CUR_BUILD)
  local out="$BENCH/objhash/$1-$2.sha256"
  if [ -f "$out" ]; then log "SKIP objhash $1-$2 (exists)"; return 0; fi
  if [ ! -d "${CUR_BUILD:-/nonexistent}" ]; then log "WARN objhash $1-$2 skipped: build dir gone (resume gap)"; return 0; fi
  log "objhash $1-$2"
  ( cd "$CUR_BUILD" && find . -name '*.o' | LC_ALL=C sort | xargs sha256sum ) > "$out"
}

# On resume: remove stale v2 build dirs from a previous invocation (everything needed
# from them was extracted immediately after each run).
for d in "$BENCH"/build2-*; do [ -d "$d" ] && rm -rf "$d"; done || true

log "START corrected benchmark; nproc=$NPROC ninja_default_jobs=$NINJA_JOBS; GGML_CCACHE=OFF on every config"

# 1. sys-true-nocache run 1
run_build sys-true-nocache 1 0 -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++

# 2. fork-pgo-true-nocache run 1
run_build fork-pgo-true-nocache 1 0 -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX"

# 3. sys-true-nocache run 2
run_build sys-true-nocache 2 0 -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++

# 4. fork-pgo-true-nocache run 2
run_build fork-pgo-true-nocache 2 0 -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX"
objhash fork-pgo-true-nocache 2

# 5. fork-cache-cold (fork -fcompile-cache, cache wiped first)
if ! has_result fork-cache-cold 1; then rm -rf "$GCC_CACHE2"; fi
run_build fork-cache-cold 1 0 \
  -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX" \
  -DCMAKE_C_FLAGS="-fcompile-cache=$GCC_CACHE2" -DCMAKE_CXX_FLAGS="-fcompile-cache=$GCC_CACHE2"
objhash fork-cache-cold 1

# 6. fork-cache-warm (keep populated cache, fresh build dir)
run_build fork-cache-warm 1 0 \
  -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX" \
  -DCMAKE_C_FLAGS="-fcompile-cache=$GCC_CACHE2" -DCMAKE_CXX_FLAGS="-fcompile-cache=$GCC_CACHE2"
objhash fork-cache-warm 1

# gcc-cache2 stats
{ du -sh "$GCC_CACHE2"; echo "files: $(find "$GCC_CACHE2" -type f | wc -l)"; } \
  > "$BENCH/logs/gcc-cache2-stats.txt"

# fork-cache-warm edge analysis (from the copied ninja log; ms per edge)
NL=$BENCH/ninjalogs/fork-cache-warm-1.ninja_log
if [ -f "$NL" ]; then
  {
    awk -F'\t' 'NR>1{d=$2-$1; tot++; if(d<500)f++; if($4 ~ /\.o$/){ot++; if(d<500)of++}} END{printf "edges_total=%d fast<500ms=%d | object_edges=%d object_fast<500ms=%d\n", tot, f, ot, of}' "$NL"
    echo "top 10 slowest edges (ms<TAB>path):"
    tail -n +2 "$NL" | awk -F'\t' '{print ($2-$1) "\t" $4}' | sort -rn | awk 'NR<=10'
  } > "$BENCH/logs/fork-cache-warm-edges.txt"
fi

# Untimed probe: re-run one real TU compile against gcc-cache2 with debug on,
# to capture a "compile-cache: ... hit/manifest-hit" line as proof (run AFTER objhash).
PROBE=$BENCH/logs/probe-fork-cache.log
if [ ! -f "$PROBE" ] && [ -d "${CUR_BUILD:-/nonexistent}" ]; then
  probe_cmd=$(ninja -C "$CUR_BUILD" -t commands | grep -F 'common.dir/common.cpp.o' | grep -F -- ' -c ' | awk 'NR==1' || true)
  if [ -n "$probe_cmd" ]; then
    { echo "PROBE build dir: $CUR_BUILD"; echo "PROBE cmd: $probe_cmd"; echo "--- output ---"; } > "$PROBE"
    if ( cd "$CUR_BUILD" && GCC_COMPILE_CACHE_DEBUG=1 sh -c "$probe_cmd" ) >> "$PROBE" 2>&1; then
      log "probe compile rc=0"
    else
      log "GUARD WARN: probe compile returned nonzero"
    fi
    if grep -E 'compile-cache:.*hit' "$PROBE" > /dev/null; then
      log "probe: fork compile-cache HIT confirmed: $(grep -E 'compile-cache:' "$PROBE" | awk 'NR<=3' | tr '\n' ' ')"
    else
      log "GUARD WARN: probe produced no compile-cache hit line — inspect $PROBE"
    fi
  else
    log "GUARD WARN: could not extract probe command"
  fi
fi

# 7. ccache-fork-cold (ccache in front of the fork wrapper; private cache dir, wiped)
export CCACHE_DIR="$CCACHE_FORK"
# pre-flight established ccache 4.9.1 auto-detects the wrapper (direct hit on 2nd compile);
# no CCACHE_COMPILERTYPE override needed.
if ! has_result ccache-fork-cold 1; then rm -rf "$CCACHE_FORK"; mkdir -p "$CCACHE_FORK"; ccache -z >/dev/null; fi
run_build ccache-fork-cold 1 1 \
  -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
if [ ! -f "$BENCH/logs/ccache-fork-stats-after-cold.txt" ]; then
  ccache -s > "$BENCH/logs/ccache-fork-stats-after-cold.txt" 2>&1
fi

# 8. ccache-fork-warm
run_build ccache-fork-warm 1 1 \
  -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
if [ ! -f "$BENCH/logs/ccache-fork-stats-after-warm.txt" ]; then
  ccache -s > "$BENCH/logs/ccache-fork-stats-after-warm.txt" 2>&1
fi
du -sh "$CCACHE_FORK" > "$BENCH/logs/ccache-fork-size.txt"

# objhash verdict: fork-nocache vs fork-cache-cold vs fork-cache-warm must be identical
A=$BENCH/objhash/fork-pgo-true-nocache-2.sha256
Bf=$BENCH/objhash/fork-cache-cold-1.sha256
C=$BENCH/objhash/fork-cache-warm-1.sha256
V=$BENCH/logs/objhash-verdict.txt
if [ -f "$A" ] && [ -f "$Bf" ] && [ -f "$C" ]; then
  if cmp -s "$A" "$Bf" && cmp -s "$Bf" "$C"; then
    echo "IDENTICAL: fork-pgo-true-nocache-2 == fork-cache-cold-1 == fork-cache-warm-1 ($(wc -l < "$A") objects)" > "$V"
  else
    { echo "MISMATCH:"
      echo "--- nocache-2 vs cache-cold:"; diff "$A" "$Bf" | awk 'NR<=30' || true
      echo "--- cache-cold vs cache-warm:"; diff "$Bf" "$C" | awk 'NR<=30' || true
    } > "$V"
  fi
else
  echo "INCOMPLETE: missing objhash file(s)" > "$V"
fi
log "objhash verdict: $(head -1 "$V")"

# Final guard: nothing recreated the global ccache dir
if [ -e /root/.cache/ccache ]; then
  log "GUARD WARN: /root/.cache/ccache exists again — investigate"
else
  log "guard ok: /root/.cache/ccache still absent"
fi

# Append corrected-environment note to ENV.md (idempotent)
if ! grep -q '^## Corrected run' "$BENCH/ENV.md"; then
  cat >> "$BENCH/ENV.md" <<EOF

## Corrected run (results2.jsonl) — $(date -Is)
- Previous results.jsonl INVALID: GGML_CCACHE defaults ON, so ggml set a global RULE_LAUNCH_COMPILE=ccache and cached every compile to /root/.cache/ccache (hence 70s "nocache" repeats).
- Fix: every config passes -DGGML_CCACHE=OFF; guards per run assert the configure log has no "compilation results will be cached" and 'ninja -t commands | grep -c ccache' == 0 for non-ccache configs (>0 for ccache-fork configs), and -fcompile-cache appears only in fork-cache configs.
- /root/.cache/ccache (created 02:10 by the invalid run) deleted before these runs; stale gcc-cache/, ccache-sys/, cachetest/ removed. New caches: gcc-cache2/ (fork -fcompile-cache), ccache-fork/ (CCACHE_DIR, ccache 4.9.1 in front of the fork wrapper).
- ccache pre-test: ccache 4.9.1 accepts the fork wrapper as-is (2nd compile = direct hit); CCACHE_COMPILERTYPE override NOT needed.
- Execution: nohup nice -n 19 ionice -c3 bash run_corrected.sh; cmake configure untimed; /usr/bin/time -v ninja (default -j = nproc+2 = $NINJA_JOBS) timed; fresh build2-* dir per run, previous run's dir deleted first.
EOF
  log "ENV.md corrected-run note appended"
fi

log "DONE"
echo DONE
