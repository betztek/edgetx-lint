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

## describe parity, 2026-11-27 (rc-config `f242d3f` + both `baseline-*` branches)

`describe.py --json` (rc-config, with `scripts/Multi.txt`) against
`edgetx-lint describe` with the same `Multi.txt` and the radio's
`RADIO/radio.yml`, field by field, locally. Counts and categories only.
Fields compared per model: name, semver, both modules (type, channels,
failsafe, multi protocol), trainer mode, timers, flight modes, every input
and mix line (channel, source, weight, offset, expo/curve, op, switch,
flight-mode mask, slow/delay, names), outputs (set, min, max, subtrim,
reversed, name), logical switches, special functions, GVar names, throttle
warning, extended limits, heli, sensor count. `baseline-pocket-2.10.0`'s
`tx16s/` is identical to `baseline-2.10.6`'s and is counted once.

| Set | Models | Backend | Equal fields | Different |
|---|---|---|---|---|
| main tx16s | 4 | v2.10.6 | 1462 | 0 |
| main tx16s | 4 | v2.12.5 | 1462 | 0 |
| main pocket | 1 | v2.10.0 | 181 | 5 |
| baseline tx16s | 16 | v2.10.6 | 3945 | 28 (+1 model with lint error) |
| baseline tx16s | 16 | v2.12.5 | 3948 | 31 (+2 models with lint errors) |
| baseline pocket | 5 | v2.10.0 | 827 | 22 |

Every difference, by category:

| Category | Count | Explanation |
|---|---|---|
| describe.py lists settings after a parse abort | 3 fields, 1 model (2.10.6) | HANDOFF finding 1. The old `model7.yml` parse abort: describe.py shows an internal module, trainer mode and a special function that EdgeTX 2.10.6 never reads. describe shows the module off and carries the `parse-aborted` error. |
| Legacy GVar numbers on 2.12 | 6 mix weights, 2 models (2.12.5) | HANDOFF finding 3. describe.py shows GV1; EdgeTX 2.12.5 stores 0. describe shows 0 plus `version-trap`. |
| describe.py drops mixed channels with default limits | 11 model output sets, 50 channels | New finding 6. describe.py only iterates `limitData` keys present in the file; a mixed channel whose limits are all default is never listed under Outputs. describe lists every mixed channel. |
| Special-function parameters in the writer's canonical form | 20 (tx16s) | describe's `def` is what the radio writes back (e.g. an extra default field); describe.py shows the file's text. Same setting (`normalized` info in `check`). |
| Expo 0 % vs none | 21 (pocket) | Representation only: describe.py prints an expo curve of 0 % as `0`, the comparison script as `none`. Same setting. |

Not exercised by the real files (0 occurrences), covered by the synthetic
`tests/fixtures/*/describe/` fixtures instead: expo GVar weights (finding 2a),
mix offset GVars (2b), GV10 (2c). Finding 4 (hand-copied pot and switch
tables): describe's names come from each backend's compiled hardware
definitions; all 26 models' source and switch names matched describe.py's
tables on today's versions. Finding 5 (2.12 file rewrite): not testable until
the radios are upgraded.
