#!/bin/bash
# perf-lab: deterministically (re)generate the benchmark inputs into OUTDIR.
#
#   gen-benches.sh OUTDIR
#
# Produces, flat in OUTDIR:
#   bench_light.cpp bench_stl.cpp bench_json.cpp big_tu.cpp   (committed, copied)
#   prelude_stl.h prelude_json.h                              (committed, copied)
#   json.hpp        nlohmann/json v3.11.3 single header, downloaded + sha256
#                   pinned (a ~900 KB third-party blob is not committed)
#   huge_tu.cpp     generated: big_tu + instantiation storm (>1 GiB GGC alloc)
#   anchor.cpp      1-line TU for the PCH-load-cost floor
#   realism/        620-header / 20-package Qt-style include fixture:
#                   60 -I roots, packages in the last 20 roots, TU with 200
#                   includes (the dir-index worst-case-ish workload)
#
# Everything is deterministic: same script -> byte-identical TUs, so numbers
# are comparable across runs and hosts.
set -euo pipefail

OUT=${1:?usage: gen-benches.sh OUTDIR}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$OUT"

# ---- 1. committed small TUs + preludes, verbatim ---------------------------
cp "$HERE"/tu/bench_light.cpp "$HERE"/tu/bench_stl.cpp \
   "$HERE"/tu/bench_json.cpp  "$HERE"/tu/big_tu.cpp \
   "$HERE"/tu/prelude_stl.h   "$HERE"/tu/prelude_json.h "$OUT/"

# ---- 2. nlohmann/json single header (pinned v3.11.3) -----------------------
JSON_SHA=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
json_ok() {
  [ -f "$OUT/json.hpp" ] && echo "$JSON_SHA  $OUT/json.hpp" | sha256sum -c - >/dev/null 2>&1
}
if ! json_ok && [ -n "${PERF_LAB_JSON_HPP:-}" ] && [ -f "${PERF_LAB_JSON_HPP:-}" ]; then
  cp "$PERF_LAB_JSON_HPP" "$OUT/json.hpp"
fi
if ! json_ok; then
  for url in \
    "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp" \
    "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"; do
    echo "fetching $url"
    if curl -fL --connect-timeout 20 -o "$OUT/json.hpp.part" "$url"; then
      mv "$OUT/json.hpp.part" "$OUT/json.hpp"
      json_ok && break
    fi
  done
fi
json_ok || {
  echo "::error::could not obtain nlohmann json.hpp v3.11.3 (sha256 $JSON_SHA); set PERF_LAB_JSON_HPP to a local copy"
  exit 1
}

# ---- 3. huge_tu.cpp: big_tu + instantiation storm --------------------------
# Port of the measurement branch's gen-huge-tu.sh (TAGS=24, DEPTH=120): pushes
# total GGC allocation past 1 GiB so the 512M-cap cell actually collects
# mid-compile while the never-collect cell does not.
TAGS=${PERF_LAB_HUGE_TAGS:-24}
DEPTH=${PERF_LAB_HUGE_DEPTH:-120}
{
  echo "// huge_tu: big_tu + generated instantiation storm (TAGS=$TAGS DEPTH=$DEPTH)."
  echo "// Goal: total GGC allocation > 1 GiB so ggc-min-heapsize=524288 still collects."
  echo '#define main bigtu_main'
  echo '#include "big_tu.cpp"'
  echo '#undef main'
  echo '#include <vector>'
  echo '#include <map>'
  echo '#include <tuple>'
  echo '#include <array>'
  echo 'namespace hstorm {'
  echo "template <int N, int Tag> struct H {"
  echo "  std::vector<std::array<char, (N % 61) + 1>> v;"
  echo "  std::map<std::array<char, (Tag % 13) + 1>, std::array<int, (N % 5) + 1>> mp;"
  echo "  std::tuple<std::array<long, (N % 7) + 1>, std::array<short, (Tag % 11) + 1>> t;"
  echo "  H<N - 1, Tag> next;"
  echo "  long f() const { return N + v.size() + mp.size() + next.f(); }"
  echo "};"
  echo "template <int Tag> struct H<0, Tag> { long f() const { return Tag; } };"
  echo "long run_all() {"
  echo "  long acc = 0;"
  for t in $(seq 1 "$TAGS"); do
    echo "  { H<$DEPTH, $t> h{}; acc += h.f(); }"
  done
  echo "  return acc;"
  echo "}"
  echo '}'
  echo 'int main() { return (bigtu_main() + int(hstorm::run_all() & 1)) & 0; }'
} > "$OUT/huge_tu.cpp"

# ---- 4. realism fixture ------------------------------------------------------
# Port of the measurement branch's mc-realism.sh: 60 -I roots each with 2 decoy
# headers; 20 Qt-ish packages of 25 headers in roots r41..r60 (620 headers
# total); TU includes headers 1..10 of every package (200 includes).
R=$OUT/realism
rm -rf "$R"
mkdir -p "$R/roots"
for i in $(seq -w 1 60); do
  d="$R/roots/r$i"
  mkdir -p "$d"
  for j in 1 2; do echo "// decoy" > "$d/impl_detail_$j.h"; done
done
for k in $(seq 1 20); do
  rk=$(printf 'r%02d' $((40 + k)))
  pk=$(printf 'Pkg%02d' "$k")
  pklower=$(printf 'pkg%02d' "$k")
  mkdir -p "$R/roots/$rk/$pk"
  for h in $(seq 1 25); do
    cat > "$R/roots/$rk/$pk/hdr$h.h" <<EOF
#ifndef ${pk}_HDR${h}_H
#define ${pk}_HDR${h}_H
namespace ${pklower} {
template <typename T> struct S$h { T v; T get() const { return v; } };
inline int f$h() { return $k * 100 + $h; }
}
#endif
EOF
  done
done
{
  for k in $(seq 1 20); do
    pk=$(printf 'Pkg%02d' "$k")
    for h in $(seq 1 10); do echo "#include <$pk/hdr$h.h>"; done
  done
  echo 'int total() { int s = 0;'
  for k in $(seq 1 20); do
    pklower=$(printf 'pkg%02d' "$k")
    for h in $(seq 1 10); do echo "  s += ${pklower}::f$h();"; done
  done
  echo '  return s; }'
} > "$R/realism_tu.cpp"

# ---- 5. tiny anchors --------------------------------------------------------
printf 'int pch_anchor;\n' > "$OUT/anchor.cpp"

echo "bench inputs generated at $OUT:"
echo "  headers in realism fixture: $(find "$R/roots" -name '*.h' | wc -l)"
echo "  huge_tu.cpp: $(wc -l < "$OUT/huge_tu.cpp") lines"
