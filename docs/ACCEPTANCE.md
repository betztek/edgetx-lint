# Real-file acceptance results

Real files from `betztek/rc-config` (attached read-only; nothing copied into
this repository). Counts and rule names only.

Run: 2026-11-24, edgetx-lint 0.1.0, backends built from EdgeTX tags v2.10.6
and v2.12.5, `--format json`, all `MODELS/*.yml` plus `RADIO/radio.yml`.

## TX16S — branch `baseline-2.10.6` (17 models + labels.yml + radio.yml)

| EdgeTX | errors | warnings | info | exit |
|---|---|---|---|---|
| v2.10.6 | 1 | 0 | 47 | 1 |
| v2.12.5 | 6 | 0 | 42 | 1 |

**v2.10.6** (expected: zero errors)

- `parse-aborted` ×1 — `model7.yml`. **Real, not a linter bug.** A quoted
  string uses a lowercase hex escape (`\x..` with a–f). EdgeTX 2.10.6's
  `YamlParser` only accepts uppercase hex digits there
  (`ps_ValEsc2/3`, fixed in 2.11+ with `toupper`) and returns `DONE_PARSING`,
  so the radio silently stops reading `model7.yml` at line 443 of 703
  (~260 lines ignored). Fix: uppercase the escape (`\x1D`) or re-save.
- info: `legacy-key` ×12 (read-only keys such as `carryTrim`, converted on
  load), `normalized` ×34 (values rewritten in the current format, e.g.
  source/switch names), `not-linted` ×1 (`labels.yml`).

**v2.12.5** (expected: version-trap on model8 + model9, 3 mixes each)

- `version-trap` ×6 — `model8.yml` ×3, `model9.yml` ×3 (`weight: -1024`,
  semver 2.9.2). Exactly as expected, nothing else unexplained.
  Correction to the premise: 2.12.5 does read `-1024` as a plain number, but
  mix weight is a 10-bit signed `SourceNumVal.value` (−512..511), so the
  radio **stores 0** (mix silently disabled), not −1024 %. Verified with
  EdgeTX's own writer (`roundtrip` prints `weight: 0`).
- `model7.yml` parses fully on 2.12.5 (the lowercase escape is accepted).
  Not on 2.11.x: EdgeTX 2.11.8 still rejects lowercase hex escapes (fixture
  `lowercase-hex-escape.yml` confirms), so the abort applies to 2.10.x and
  2.11.x alike.
- info: `legacy-key` ×13, `normalized` ×28 (e.g. 2.9 pot/slider names
  `S1`/`LS` → `P1`/`SL1`, custom-function parameter format), `not-linted` ×1.

## Pocket — branch `baseline-pocket-2.10.0` (5 models + radio.yml)

| EdgeTX | errors | warnings | info | exit |
|---|---|---|---|---|
| v2.10.6 | 0 | 0 | 5 | 0 |
| v2.12.5 | 0 | 0 | 5 | 0 |

- info only: `legacy-key` ×5. No weights in the legacy GVar range.

## Not covered

- `postModelLoad()` fix-ups the radio applies after loading (they touch
  hardware/runtime state) are not simulated.
- Companion's codec (tier 1b) is not compared yet; see README.

## Update 2026-11-26: rc-config `main` after pruning (`ea98a86`)

TX16S: 4 models + labels.yml + radio.yml. Pocket: 1 model + radio.yml.

| Radio | EdgeTX | errors | warnings |
|---|---|---|---|
| tx16s | v2.10.6 | 0 | 0 |
| tx16s | v2.12.5 | 0 | 0 |
| pocket | v2.10.0 | 0 | 0 |
| pocket | v2.12.5 | 0 | 0 |

The models with findings above (model7, model8, model9) were deleted.
`v2.10.0` is the Pocket's actual firmware; it behaves the same as v2.10.6 on
every fixture.
