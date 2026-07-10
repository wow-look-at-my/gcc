# Benchmark environment

- date: 2026-07-04T02:45:24+00:00
- nproc: 4 (ninja default -j = nproc+2 = 6; all runs used ninja default)
- kernel: 6.18.5
- RAM: 15Gi
- system gcc: gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- fork gcc (PGO tree): xgcc (GCC) 14.4.1 20260626
- ccache: ccache version 4.9.1
- cmake: cmake version 3.28.3; ninja: 1.11.1
- llama.cpp: 7f4cced (claude/ecstatic-archimedes-92x4g7 (#15))
- fork gcc source HEAD: 40b19a60a
- build type: Release, -G Ninja, LLAMA_CURL=OFF, defaults otherwise
- fork compile cache: xattr metadata (ext4, user.gcc_cc.* attrs verified); -fintegrated-as not needed (default-on)

## Corrected run (results2.jsonl) — 2026-07-04T08:18:17+00:00
- Previous results.jsonl INVALID: GGML_CCACHE defaults ON, so ggml set a global RULE_LAUNCH_COMPILE=ccache and cached every compile to /root/.cache/ccache (hence 70s "nocache" repeats).
- Fix: every config passes -DGGML_CCACHE=OFF; guards per run assert the configure log has no "compilation results will be cached" and 'ninja -t commands | grep -c ccache' == 0 for non-ccache configs (>0 for ccache-fork configs), and -fcompile-cache appears only in fork-cache configs.
- /root/.cache/ccache (created 02:10 by the invalid run) deleted before these runs; stale gcc-cache/, ccache-sys/, cachetest/ removed. New caches: gcc-cache2/ (fork -fcompile-cache), ccache-fork/ (CCACHE_DIR, ccache 4.9.1 in front of the fork wrapper).
- ccache pre-test: ccache 4.9.1 accepts the fork wrapper as-is (2nd compile = direct hit); CCACHE_COMPILERTYPE override NOT needed.
- Execution: nohup nice -n 19 ionice -c3 bash run_corrected.sh; cmake configure untimed; /usr/bin/time -v ninja (default -j = nproc+2 = 6) timed; fresh build2-* dir per run, previous run's dir deleted first.
