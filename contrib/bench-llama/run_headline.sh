#!/usr/bin/env bash
# Headline compile-time benchmark: fork gcc (PGO) vs system gcc building llama.cpp.
# Appends one JSON line per completed run to results.jsonl. Resumable: runs already
# present in results.jsonl are skipped (append-only).
set -euo pipefail

BENCH=/tmp/claude-0/-home-user/b5431e56-3b74-5bd3-89b4-e7d9cbd67820/scratchpad/bench
LLAMA=/home/user/llama.cpp
RESULTS=$BENCH/results.jsonl
NPROC=$(nproc)
NINJA_JOBS=$((NPROC + 2))   # ninja default -j = nproc+2
FORK_CC=$BENCH/bin/fork-pgo-cc
FORK_CXX=$BENCH/bin/fork-pgo-cxx
GCC_CACHE=$BENCH/gcc-cache
CCACHE_SYS=$BENCH/ccache-sys

mkdir -p "$BENCH/logs" "$BENCH/ninjalogs" "$BENCH/objhash"
touch "$RESULTS"

PREV_BUILD=""

log() { echo "[$(date -Is)] $*"; }

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

run_build() { # config n [extra cmake args...]
  local config="$1" n="$2"; shift 2
  local bdir="$BENCH/build-$config-$n"
  local log_f="$BENCH/logs/$config-$n.log"
  local cfglog="$BENCH/logs/$config-$n.configure.log"

  if has_result "$config" "$n"; then
    log "SKIP $config run $n (already in results.jsonl)"
    return 0
  fi

  log "=== $config run $n : configure ==="
  if [ -n "$PREV_BUILD" ] && [ -d "$PREV_BUILD" ]; then rm -rf "$PREV_BUILD"; fi
  rm -rf "$bdir"
  cmake -S "$LLAMA" -B "$bdir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF "$@" > "$cfglog" 2>&1

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

# On resume: remove stale build dirs from a previous invocation (everything needed
# from them was extracted immediately after each run).
for d in "$BENCH"/build-*; do [ -d "$d" ] && rm -rf "$d"; done || true

log "START headline benchmark; nproc=$NPROC ninja_default_jobs=$NINJA_JOBS"

# 1. sys-nocache run 1
run_build sys-nocache 1 -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++

# 2. fork-pgo-nocache run 1
run_build fork-pgo-nocache 1 -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX"
objhash fork-pgo-nocache 1

# 3. sys-nocache run 2
run_build sys-nocache 2 -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++

# 4. fork-pgo-nocache run 2
run_build fork-pgo-nocache 2 -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX"
objhash fork-pgo-nocache 2
# Smoke test fork-built binary BEFORE this dir is deleted
if [ ! -f "$BENCH/logs/smoke.txt" ] && [ -d "${CUR_BUILD:-/nonexistent}" ]; then
  {
    echo "== $CUR_BUILD/bin/llama-cli --version =="
    "$CUR_BUILD/bin/llama-cli" --version 2>&1 || echo "(exit $?)"
    echo "== ldd bin/llama-cli | grep stdc++ =="
    ldd "$CUR_BUILD/bin/llama-cli" | grep stdc++ || true
  } > "$BENCH/logs/smoke.txt" 2>&1
  log "smoke test saved to logs/smoke.txt"
fi

# 5. fork-pgo-cache-cold (empty cache)
if ! has_result fork-pgo-cache-cold 1; then rm -rf "$GCC_CACHE"; fi
run_build fork-pgo-cache-cold 1 \
  -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX" \
  -DCMAKE_C_FLAGS="-fcompile-cache=$GCC_CACHE" -DCMAKE_CXX_FLAGS="-fcompile-cache=$GCC_CACHE"
objhash fork-pgo-cache-cold 1

# 6. fork-pgo-cache-warm (keep populated cache, fresh build dir)
run_build fork-pgo-cache-warm 1 \
  -DCMAKE_C_COMPILER="$FORK_CC" -DCMAKE_CXX_COMPILER="$FORK_CXX" \
  -DCMAKE_C_FLAGS="-fcompile-cache=$GCC_CACHE" -DCMAKE_CXX_FLAGS="-fcompile-cache=$GCC_CACHE"
objhash fork-pgo-cache-warm 1
if [ ! -f "$BENCH/logs/gcc-cache-stats.txt" ]; then
  { du -sh "$GCC_CACHE"; echo "files: $(find "$GCC_CACHE" -type f | wc -l)"; } \
    > "$BENCH/logs/gcc-cache-stats.txt"
  log "gcc-cache stats saved"
fi

# 7. ccache-sys-cold
export CCACHE_DIR="$CCACHE_SYS"
if ! has_result ccache-sys-cold 1; then rm -rf "$CCACHE_SYS"; mkdir -p "$CCACHE_SYS"; ccache -z >/dev/null; fi
run_build ccache-sys-cold 1 \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
if [ ! -f "$BENCH/logs/ccache-stats-after-cold.txt" ]; then
  ccache -s > "$BENCH/logs/ccache-stats-after-cold.txt" 2>&1
fi

# 8. ccache-sys-warm
run_build ccache-sys-warm 1 \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
if [ ! -f "$BENCH/logs/ccache-stats-after-warm.txt" ]; then
  ccache -s > "$BENCH/logs/ccache-stats-after-warm.txt" 2>&1
fi
du -sh "$CCACHE_SYS" > "$BENCH/logs/ccache-sys-size.txt"

# Environment summary
{
  echo "# Benchmark environment"
  echo
  echo "- date: $(date -Is)"
  echo "- nproc: $NPROC (ninja default -j = nproc+2 = $NINJA_JOBS; all runs used ninja default)"
  echo "- kernel: $(uname -r)"
  echo "- RAM: $(free -h | awk '/^Mem:/{print $2}')"
  echo "- system gcc: $(/usr/bin/gcc --version | head -1)"
  echo "- fork gcc (PGO tree): $($FORK_CC --version | head -1)"
  echo "- ccache: $(ccache --version | head -1)"
  echo "- cmake: $(cmake --version | head -1); ninja: $(ninja --version)"
  echo "- llama.cpp: $(git -C $LLAMA rev-parse --short HEAD) ($(git -C $LLAMA log -1 --format=%s | head -c 60))"
  echo "- fork gcc source HEAD: $(git -C /home/user/gcc rev-parse --short HEAD)"
  echo "- build type: Release, -G Ninja, LLAMA_CURL=OFF, defaults otherwise"
  echo "- fork compile cache: xattr metadata (ext4, user.gcc_cc.* attrs verified); -fintegrated-as not needed (default-on)"
} > "$BENCH/ENV.md"

log "DONE"
echo DONE
