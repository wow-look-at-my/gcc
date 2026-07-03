# perf-train -- PGO training inputs for the compiler proper

Small, deterministic, self-contained translation units used as the profile
workload when building cc1/cc1plus with profile-guided optimization
(`-fprofile-generate` build, compile these, `-fprofile-use` rebuild).  They
are consumed by the `package-pgo` job in `.github/workflows/ci.yml`.

| File | Trains | Shape |
|------|--------|-------|
| `train-stl.cpp` | cc1plus | Header-heavy realistic STL TU: containers, algorithms, `std::function`, several class templates x several types. Compiled at `-O0` and `-O2`. |
| `train-templates.cpp` | cc1plus | `<regex>` + multi-container churn + a recursive class-template instantiation storm (hundreds of distinct specializations) -- keeps tsubst/coercion/specialization-table paths hot. Compiled at `-O2`. |
| `train-c.c` | cc1 | Plain C: structs, loops, switches, function pointers, varargs formatting. Compiled at `-O0` and `-O2`. |

Ground rules for these files:

- **Self-contained.**  Standard headers only -- no third-party single-header
  libraries, no fixture directories.  They must compile with just the
  freshly built compiler and its own libstdc++ headers.
- **Deterministic.**  No `__TIME__`/randomness; the same compiler always
  sees the same work.
- **Moderate size.**  The instrumented compiler is ~2-3x slower than a
  regular build; the whole training run should stay in the low minutes.
- Training compiles use `-c` only, so target libstdc++ *headers* suffice
  (no built `libstdc++.so` needed at train time).

Measured effect of PGO built from this class of workload (Xeon 2.1 GHz,
median of 7): ~-8% wall on a large template-heavy TU at `-O2`, -5..-10% on
mid-size STL TUs; compiler outputs byte-identical to the non-PGO build.
Adding LTO on top measured no consistent further gain (~+/-1.5%) for
considerably more build time and disk, so the CI recipe uses plain PGO.
