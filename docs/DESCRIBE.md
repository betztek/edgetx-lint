# `edgetx-lint describe`: facts JSON (schema 1)

`describe` prints what the radio **loads** from a model, as JSON, for
documentation generators such as rc-config's `build_docs.py`. It replaces
rc-config's interim `scripts/describe.py` (see `HANDOFF.md` → "describe: why
and design" for the reasons).

```sh
edgetx-lint describe --radio tx16s --edgetx v2.10.6 [--multi Multi.txt] \
    RADIO/radio.yml MODELS/model*.yml > facts.json
```

| Option | Meaning |
|---|---|
| `--radio`, `--edgetx` | Pick the backend, as for `check`. Use the version the radio runs. |
| `RADIO/radio.yml` | Optional, at most one, any position. Loaded first: switch names and types, pot labels and stick mode come from it. Without it, EdgeTX's default radio settings are used and `"radio"` is `null`. |
| `--multi FILE` | The SD card's `Multi.txt`, to name Multi-protocol module protocols. The list lives on the SD card, not in EdgeTX. Without it `protocol.text` is empty for multi modules. |
| `--strict` | Exit 1 on lint warnings too. |
| `--compact` | One-line JSON. |

## Where the facts come from

- **State:** each file goes through `check` first (tier 1 + tier 2). The facts
  are read from what EdgeTX's own loader (`readModelYaml()` /
  `loadRadioSettingsYaml()`) left in memory. Keys the radio ignores, lines
  after a parse abort and clamped values never reach that state, so they
  can't be documented.
- **`raw`:** the token EdgeTX's own YAML writer emits for that field of the
  loaded state (the schema's writer callback, called on the field). It is
  what the radio writes back on save, not necessarily what the file says.
- **`text`:** EdgeTX's UI helpers (`getSourceString`,
  `getSwitchPositionName`, `getCurveString`, `getSwitchName`,
  `getPotLabel`), so names match the radio screen and the board's hardware
  definitions for that EdgeTX version. The radio font's icon glyphs are
  mapped: arrows become `↑ ↓ ← →`, type icons are dropped (`kind` says what a
  reference is).
- **Legacy numbers are not guessed.** A ≤2.10 file whose mix weight is
  `-1024` (GV1 in the old encoding) shows `GV1` on a 2.10 backend, and the
  stored `0` on a 2.12 backend, with the `version-trap` error in `lint`.

## Lint gate

describe never refuses to print, and never prints values the radio ignores:

- Every model carries its `check` result in `lint` (counts and findings).
- If EdgeTX's loader fails, `loaded` is `false` and only `file`, `kind`,
  `semver`, `loaded` and `lint` are present.
- Exit status: 0 clean; **1 if any input has a lint error** (or a warning
  with `--strict`); 2 usage/internal error. Callers must treat exit 1 as
  "the docs for these files describe a partly ignored file": show the
  findings, don't publish silently. rc-config's lint job is blocking, so
  this only happens there when the docs and lint jobs disagree.

## Schema 1

Additive changes (new keys) keep `schema: 1`. Renaming, removing or changing
the meaning of a key bumps it.

```text
{
  "schema": 1,
  "tool":   {"name", "version", "radio", "edgetx"},
  "radio":  RADIO | null,
  "models": [MODEL, ...]           // input order, radio.yml excluded
}
```

### Common shapes

| Shape | Keys |
|---|---|
| `SWITCH` | `raw`, `text`, `kind` (`none` `switch` `multipos` `trim` `logical` `on` `one` `flight_mode` `telemetry` `other`), `negated` (only if true). `switch`: `switch` (name), `position` (`up` `middle` `down`). `logical`/`flight_mode`: `index` (L: 1-based, FM: 0-based). `multipos`: `position` (1-based). |
| `SOURCE` | `raw`, `text`, `kind` (`none` `input` `lua` `stick` `pot` `constant` `heli` `trim` `switch` `logical` `trainer` `channel` `gvar` `timer` `telemetry` `other`), `negated`, `index` (1-based, for `input` `channel` `gvar` `logical`). |
| `VALUE` | A number that may hold a GVar or (2.11+) a source. `raw`, `text`; then either `value` (number, unit applied) or `gvar` (1-based) + `negated`, or (2.11+) `source` (`SOURCE`). |
| `FMMASK` | `mask` (bit i set = off in FM i), `active_in` (list of FM numbers), `text` (`all`, `none`, `FM0,FM2`). |
| `CURVE` | `type` (`diff` `expo` `func` `custom`), `value` (`VALUE` for diff/expo, number otherwise), `text` (`""` when diff/expo 0). |
| `LINT` | `errors`, `warnings`, `infos`, `findings`: list of `severity`, `rule`, `line`, `path`, `message`, `hint`? (same as `check --format json`). |

### RADIO

`file`, `kind: "radio"`, `semver`, `loaded`, `lint`, `stick_mode` (1–4),
`switches` (all hardware switch slots: `name`, `type` = `none` `toggle`
`2pos` `3pos`, as configured in radio.yml), `pots` (`index`, `name` as
written in YAML, e.g. `P1`, `label` as shown, e.g. `S1`).

### MODEL

| Key | Content |
|---|---|
| `file`, `kind: "model"`, `semver`, `loaded`, `lint` | As above. `semver` is who wrote the file (write-only in EdgeTX). |
| `name`; `labels`, `bitmap` | Header. `labels`/`bitmap` only on colour radios. |
| `modules[]` | Every module slot: `index`, `slot` (`internal` `external`), `type` (writer token, e.g. `TYPE_MULTIMODULE`), `enabled`. If enabled: `protocol` (`raw` e.g. `"6,1"`, `protocol`/`subtype` numbers for multi, `text`), `channels` (`start` and `count`, 1-based start, `text` `CH1-CH8`), `failsafe` (token). |
| `trainer_mode` | Token, e.g. `OFF`, `MASTER_MULTI`. |
| `timers[]` | Only timers whose mode isn't `OFF`: `index` (1-based), `name`, `mode`, `switch` (`SWITCH`), `start_s`, `countdown_beep`, `minute_beep`, `persistent`. |
| `flight_modes[]` | FM0 and every FM with a name or switch: `index` (0-based), `name`, `switch` (empty for FM0), `fade_in_s`, `fade_out_s`. |
| `inputs[]` | Expo lines in radio order: `line`, `input` (1-based), `input_name`, `name` (line name), `source`, `weight`, `offset` (`VALUE`, %), `curve` (`CURVE`), `switch`, `flight_modes` (`FMMASK`), `side` (`both` `positive` `negative`). |
| `mixes[]` | Mix lines in radio order: `line`, `channel` (1-based), `channel_name`, `name`, `source`, `weight`, `offset`, `curve`, `op` (`ADD` `MUL` `REPL`), `switch`, `flight_modes`, `include_trim`, `slow_up_s`, `slow_down_s`, `delay_up_s`, `delay_down_s`, `warning`. |
| `outputs[]` | Channels that are mixed or have non-default limits (a name alone doesn't count): `channel`, `name`, `min`, `max`, `subtrim` (`VALUE`, % of travel), `ppm_center_us`, `symmetrical`, `reversed`, `curve` (`value` index, `text`), `mixed`. |
| `logical_switches[]` | Used ones: `index` (1-based), `func`, `def` (writer tokens), `v1`/`v2` decoded by family (switches for AND/OR/XOR/sticky, sources for comparisons, source + raw number for a/x tests, none for timers), `and_switch`, `delay_s`, `duration_s`. |
| `special_functions[]` | Used ones: `index` (1-based), `switch`, `func`, `def` (writer tokens), `enabled`. |
| `global_variables[]` | GVars with a name or a non-zero value: `index` (1-based), `name`, `unit` (`%` or `""`), `prec`, `values[]` (`fm` + `value`, or `fm` + `uses_fm`; FMs that just inherit FM0 are left out). |
| `throttle_warning_disabled`, `switch_warning`, `rssi_alarms` (`disabled`, `warning`, `critical`), `telemetry_sensors[]` (`index`, `label`), `extended_limits`, `helicopter` | As loaded. |

## Not (yet) covered

Curves' points, telemetry sensor details, Lua scripts, colour-screen
widgets/layouts, the trainer channel map, and `postModelLoad()` fix-ups (not
simulated, same as `check`). Add keys as rc-config's docs need them.
