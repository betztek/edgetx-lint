# edgetx-lint — notes for Claude

CLI + GitHub Action that validates EdgeTX model/radio YAML **with EdgeTX's own
parser, loader and writer**, and reports settings the radio would silently
ignore or misread.

## Hard constraints

- **Zero patches to `third_party/edgetx`.** We only wrap the stock
  `YamlParserCalls` table (`YamlTreeWalker::get_parser_calls()`) and call
  EdgeTX's public functions. If a patch ever looks unavoidable: stop and
  report why instead.
- **Never load or dump EdgeTX YAML with a generic YAML library** (PyYAML,
  ruamel, yaml-cpp...). They misread `ON`/`OFF` etc. Compare files as bytes or
  through EdgeTX's parser. Python tests use only `json` + string ops.
- **Fixtures are synthetic.** They come from `dump-default` (EdgeTX's own
  writer) plus one deliberate edit each (`tests/fixtures/generate.py`).
  Never copy, quote or commit files from betztek/rc-config or any real user.
  `docs/ACCEPTANCE.md` holds counts and rule names only.
- Fixtures/goldens are byte-exact (`.gitattributes`: `-text`); the radio
  rewrites whole files on save, so round-trip diffs are structural, not
  textual.
- `semver` is write-only in EdgeTX: no version conversion happens on load.
- Exit codes: 0 clean, 1 findings (errors; warnings only with `--strict`),
  2 usage/internal error.

## Architecture

- `CMakeLists.txt` adds the EdgeTX tree with `NATIVE_BUILD=ON
  DISABLE_COMPANION=ON PCB=… PCBREV=…` and links the backend against EdgeTX's
  own `radiolib_native` + `simu_drivers` object libraries (what
  `gtests-radio` links). One backend per (radio, EdgeTX tag):
  `edgetx-lint-<radio>-<tag>`. Struct layouts are compile-time per board.
- Radios: `tx16s` = `PCB=X10 PCBREV=TX16S` (`yaml_datastructs_x10.cpp`);
  `pocket` = `PCB=X7 PCBREV=POCKET` (`yaml_datastructs_128x64.cpp`).
- `src/frontend/main.cpp`: dispatcher `edgetx-lint`; finds backends in
  `$EDGETX_LINT_BACKEND_DIR`, `<bin>/../libexec/edgetx-lint`, … and execs.
- `src/backend/linter.cpp`:
  - Tier 1: instrumented parse, one line per `YamlParser::parse()` call →
    real line numbers. Unknown keys (did-you-mean from sibling tags + schema
    index for misplaced keys), wrong nesting, bad enum/number, custom-reader
    "not understood" check, bad index, duplicate key, parser abort.
  - Tier 2: `readModelYaml()` / `loadRadioSettingsYaml(false)` via simu
    FatFs on a temp copy, then `YamlTreeWalker::generate()` with a recording
    writer. Input and output values are matched by
    (schema node, bit offset, walker element-index chain) and classified:
    legacy-key (read-only `YAML_CUSTOM(..., r_*, nullptr)`), inactive
    (`*_is_active` / union member), dropped, value-lost, out-of-range,
    normalized. Values stored as zero are lossless (writer omits empty).
  - version-trap: mix/expo weight/offset/curve value numbers in the ≤2.10
    GVar encoding range, file semver vs backend version on opposite sides of
    2.11. On 2.12 these fields are 10-bit (`SourceNumVal`), so -1024 is
    stored as 0.
  - radio.yml: checksum (radio discards the file on mismatch unless
    `manuallyEdited`), board/flavour mismatch.
- EdgeTX TRACEs go to stdout, even from static constructors: the backend
  redirects fd 1 in a `constructor(101)` and prints only to the saved fd.
  `EDGETX_LINT_TRACE=1` shows EdgeTX's traces on stderr.
- Version differences are handled in code with `ETX_VERSION_NUM` (from
  EdgeTX's `stamp.h`), `__has_include`, and SFINAE (`enumHasActive`), never by
  touching EdgeTX.

## Build & test

```sh
scripts/setup-deps.sh                      # Ubuntu 24.04: cmake, ninja, libclang, pillow, lz4...
git submodule update --init --depth 1 third_party/edgetx
scripts/checkout-edgetx.sh v2.12.5         # any tag; pulls EdgeTX's nested submodules
scripts/build.sh tx16s                     # -> dist/bin/edgetx-lint + dist/libexec/edgetx-lint/*
scripts/build.sh pocket
python3 tests/run_tests.py --bin-dir dist/bin
python3 tests/run_tests.py --bin-dir dist/bin --update   # rewrite goldens (review the diff!)
```

A second EdgeTX version can be built side by side from another checkout:
`scripts/build.sh tx16s /path/to/edgetx-v2.10.6` (clone with
`--recurse-submodules --shallow-submodules`). TX16S takes ~2 min on 4 cores,
Pocket ~40 s.

Prefer the prebuilt release in cloud sessions: `scripts/install.sh latest`.

Gotchas: EdgeTX ≤2.10's generator only finds `libclang.so` in `/usr/lib` or
`/usr/local/lib` (setup-deps symlinks it). Editing a source while ninja is
compiling it can leave a stale object; `touch` and rebuild.

## Usage

```sh
edgetx-lint check --radio tx16s --edgetx v2.12.5 MODELS/*.yml RADIO/radio.yml
edgetx-lint check --radio pocket --format json model01.yml
edgetx-lint check --radio tx16s --format github --warn-unknown ...
edgetx-lint list
<backend> dump-default model|radio     # EdgeTX's own defaults (fixture source)
<backend> roundtrip FILE               # what the radio writes back after loading
```

## Out of scope (later)

`describe` subcommand (port betztek/rc-config scripts/describe.py), mixer
behaviour tests (EdgeTX PR #7220 scenarios), simulator screenshots (upstream
PR #7668), macOS/Windows builds, Companion codec cross-check (tier 1b, see
README).
