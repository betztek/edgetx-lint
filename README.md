# edgetx-lint

Validate EdgeTX model and radio-settings YAML **with EdgeTX's own parser,
loader and writer**, and report settings the radio would silently ignore or
misread.

Community tool, not affiliated with the EdgeTX project.

## Why

EdgeTX never rejects a bad key. In `radio/src/storage/yaml/yaml_parser.cpp`
an unknown key makes `find_node()` return false and the radio only TRACEs it:
the setting is silently dropped. Values are just as quiet: an unknown enum
becomes the default, a number that does not fit is truncated, and some
encodings changed between releases with no conversion (`semver` is
write-only). Example: a 2.9-era model with `weight: -1024` (meaning GV1 up to
2.10) loads on 2.12.5 as a plain number that does not fit the 10-bit field and
is stored as `0` — the mix is silently dead.

edgetx-lint compiles EdgeTX's real storage code for each radio and EdgeTX
release and runs your files through it.

## Usage

```sh
edgetx-lint check --radio tx16s --edgetx v2.12.5 MODELS/*.yml RADIO/radio.yml
edgetx-lint check --radio pocket --format json MODELS/model01.yml
edgetx-lint list        # installed radio / EdgeTX backends
```

| option | |
|---|---|
| `--radio tx16s\|pocket` | required |
| `--edgetx TAG` | EdgeTX release, default newest installed |
| `--format text\|json\|github` | `github` prints `::error` annotations |
| `--warn-unknown` | unknown keys become warnings |
| `--strict` | warnings also fail |
| `--quiet` | hide info findings |
| `--type model\|radio` | override detection (`radio.yml` / `board:` key → radio) |
| `--no-roundtrip` | skip tier 2 |

Exit status: `0` clean, `1` findings (any error; warnings with `--strict`),
`2` usage or internal error.

Output (text):

```
MODELS/typo.yml:41: error [unknown-key] mixData[1].wieght: unknown key 'wieght': the radio silently ignores it
    hint: did you mean 'weight'?
```

### Rules

| rule | severity | meaning |
|---|---|---|
| `unknown-key` | error (`--warn-unknown`: warning) | key not in the radio's schema here; hint: did-you-mean and where else it is valid |
| `wrong-nesting` | error | key indented under a plain value |
| `bad-value` | error | enum/number/source the radio does not understand (stores a fallback) |
| `out-of-range` | error | number does not fit the field; radio stores something else |
| `bad-index` | error | array index beyond the radio's limit; whole entry dropped |
| `parse-aborted` | error | EdgeTX's parser stops; the rest of the file is ignored |
| `version-trap` | error | numeric weight/offset in the ≤2.10 GVar encoding, read across the 2.11 boundary |
| `checksum` | error | `radio.yml` checksum mismatch without `manuallyEdited`: radio discards the file |
| `wrong-radio` | error | `radio.yml` `board:` differs from `--radio` |
| `load-failed` / `io-error` | error | file cannot be loaded/read |
| `duplicate-key` | warning | later value overrides an earlier one |
| `value-lost` | warning | value read as NONE/0/empty and lost on save |
| `inactive` | warning | valid but context-gated (`*_is_active`, unselected union member); dropped on save |
| `dropped` | warning | non-default value the radio does not write back |
| `string-truncated` | warning | string longer than the field |
| `ignored-value` | warning | value given for a section key |
| `no-semver` | warning | no `semver`: version-dependent checks impossible |
| `legacy-key` | info | read-only legacy key (`YAML_CUSTOM(..., r_*, nullptr)`), converted on load |
| `normalized` | info | radio rewrites the value in the current format (same setting) |

### How it works

- **Tier 1** — EdgeTX's `YamlParser` with the stock `YamlTreeWalker`
  callback table wrapped, not replaced. The parser is fed one line per call,
  so findings carry real line numbers and key paths.
- **Tier 2** — EdgeTX's `readModelYaml()` / `loadRadioSettingsYaml()` load a
  temp copy, `YamlTreeWalker::generate()` writes it back, and every input
  value is matched to what the radio would write. The radio rewrites whole
  files on save, so this is a structural diff, not a text diff.
- One backend binary per (radio, EdgeTX tag): storage layout is compile-time
  per board. The `edgetx-lint` dispatcher picks the backend.
- Built by reusing EdgeTX's native/simu CMake build (`radiolib_native` +
  `simu_drivers`, the same objects `gtests-radio` links). No patches to EdgeTX.

Supported: `tx16s` (`PCB=X10 PCBREV=TX16S`), `pocket` (`PCB=X7
PCBREV=POCKET`); EdgeTX v2.10.6, v2.10.7, v2.11.8 and v2.12.5 are built and
tested in CI and shipped in the release. Other tags usually build unchanged
(`scripts/build.sh RADIO /path/to/edgetx`).

Releases: push a `v*` tag, or run the Release workflow manually on `main`
with input `tag` (it creates the tag). `VERSION` must match.

## Install

Prebuilt linux-x64 (dispatcher + all backends):

```sh
scripts/install.sh latest            # -> ~/.local/edgetx-lint/bin/edgetx-lint
```

From source (Ubuntu 24.04): see [CLAUDE.md](CLAUDE.md#build--test).

## GitHub Action

```yaml
- uses: betztek/edgetx-lint@v0.1.0
  with:
    radio: tx16s
    edgetx: v2.12.5
    files: "tx16s/MODELS/*.yml tx16s/RADIO/radio.yml"
    args: --quiet
```

The action downloads the matching release asset and annotates the PR.

## Not yet

- **Companion cross-check (tier 1b, EdgeTX issue #3362)** — deferred. Building
  Companion's `firmwares` codec requires the full Companion CMake: Qt6 Core,
  Widgets, Multimedia, PrintSupport, SerialPort, Svg, Xml, plus `rs_dfu`
  (Rust/cargo) and yaml-cpp. Estimate: +1–2 days of work, +10–15 min CI per
  EdgeTX tag, and a ~1 GB toolchain. Planned approach: load with Companion,
  write with Companion, then diff both through the radio parser with the
  existing tier-2 matcher.
- `postModelLoad()` fix-ups are not simulated.
- `describe` subcommand, mixer behaviour tests, simulator screenshots,
  macOS/Windows builds.

## License

GPL-2.0, same as EdgeTX.
