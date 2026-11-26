# Handoff: state and next steps

Read this and `CLAUDE.md` before starting work in a new session. Last updated
2026-11-26.

## State

| Item | State |
|---|---|
| `main` | PR #1 (engine, fixtures, CI, Action) and PR #3 (EdgeTX v2.10.7 + v2.11.8, release by dispatch) merged |
| Release `v0.1.0` | Published: `edgetx-lint-linux-x64.tar.gz` + `.sha256`, 8 backends (tx16s, pocket × v2.10.6, v2.10.7, v2.11.8, v2.12.5). **Its Action is broken** (see PR #4). The tarball and `scripts/install.sh v0.1.0` work. |
| PR #4 | Open. Fixes the `action.yml` parse error and the exit code under `bash -e`. Adds the CI `action` job and EdgeTX v2.10.0 (the Pocket's firmware). Bumps `VERSION` to 0.1.1. |
| Ruleset on `main` | Set by the owner: PR required + `ci-ok` check |
| CI | Matrix v2.10.0, v2.10.6, v2.10.7, v2.11.8, v2.12.5 (about 4 min); `action` job; `ci-ok` aggregates both |
| rc-config CI | Not enabled. A ready workflow is in `docs/rc-config/lint.yml` (see below). |

## Next steps, in order

1. **Merge PR #4** once `ci-ok` is green (owner says "merge"). Merge commit, no
   squash.
2. **Release v0.1.1.** Run the Release workflow on `main` with input
   `tag: v0.1.1` (`mcp__github__actions_run_trigger`, `run_workflow`,
   `workflow_id: release.yml`, `ref: main`). It builds 10 backends, creates the
   tag, publishes the tarball and smoke-tests the published Action. Then
   check the run: every job green, especially `action-smoke-test`.
3. **Enable lint in rc-config.** Copy `docs/rc-config/lint.yml` over
   rc-config's `.github/workflows/lint.yml` in an rc-config PR. It needs
   v0.1.1 to exist. edgetx-lint is public, so rc-config's default
   `GITHUB_TOKEN` can download the release; no secret is needed. This
   session only had rc-config read-only, so the owner or a session started
   with rc-config push access does this. Afterwards, consider adding the
   `edgetx (…)` blocking checks and `docs up to date` to rc-config's ruleset.
4. **Optional:** delete the stale tag `v0.1.0` and its release, or leave them.
   Its Action never worked; the tarball is fine. Owner only: sessions can't
   push or delete tags.
5. **`describe` subcommand.** See "describe: why and design" below. Start
   after step 3, so rc-config lint is already guarding model edits.

## describe: why and design

rc-config builds its per-aircraft docs from `scripts/describe.py`, which calls
itself interim "until `edgetx-lint describe`". This is not a language port.
The point is to move the documentation's source of truth from what the file
says to what the radio loads. A faithful port would keep the bugs below.

### Why (findings from building edgetx-lint, checked 2026-11-26 against rc-config `ea98a86`)

1. **It documents the text, not what the radio loads.** describe.py reads
   every line with `yaml.BaseLoader`. The radio drops unknown keys and stops
   at a parse abort. Docs built from the old TX16S model7 would have listed
   settings after the line where EdgeTX 2.10.6 stopped reading.
2. **Its GVar weight decoding is wrong in three places:**
   - **Expo weights:** it applies the 11-bit mix range (GV1 = −1024) to
     input/expo weights. In 2.10 these are 8-bit (`GV1_SMALL`, GV1 = −128),
     so expo GVars show as plain numbers.
   - **Mix offsets:** they use the same legacy encoding (13-bit field, large
     range) but are passed through undecoded.
   - **GV10:** it accepts −1015 / 1015 as GV10. Only GV1–GV9 exist.
3. **Its 2.12 assumption is wrong.** It assumes 2.12 reads legacy numbers as
   literal weights. Mix weight in 2.12 is a 10-bit `SourceNumVal`, so −1024 is
   stored as 0. Only EdgeTX's loader gets that right. The `semver` argument
   it takes is never used.
4. **Hand-copied hardware tables drift.** Pot labels and the Pocket's
   physical switch types are copied from EdgeTX sources per board. The
   backends have them compiled in, per board and per EdgeTX version.
5. **Its decoding rules only cover EdgeTX 2.9/2.10 on the TX16S.** After the
   2.12 upgrade rewrites the files (e.g. pots `S1` → `P1`), describe.py needs
   new rules. edgetx-lint only needs another backend.

### Design

- **Split.** `edgetx-lint describe --radio R --edgetx T FILE...` emits facts
  JSON only, with a `"schema": 1` field. rc-config's `build_docs.py` keeps the
  Markdown rendering and the `planes/*.notes.yml` prose. The radio's view and
  the documentation layer stay separate.
- **Source.** Facts come from the tier-2 state: the model after EdgeTX's
  `readModelYaml()`, i.e. exactly what the radio holds. Display names
  (sources, switch positions, curves, GVars) should come from EdgeTX's own
  helpers so they match the radio UI: `getSourceString`,
  `getSwitchPositionName`, `getCurveString` and `getGVarString` in
  `strhelpers.h` (present in 2.10.6 and 2.12.5; not yet tried from the
  backend).
- **Lint gate.** describe runs the linter first. If a file has errors, it
  either refuses (exit 1) or marks the affected values `"loaded": false`.
  Pick one when implementing and document it. Never silently document values
  the radio ignores.
- **Legacy numbers are shown as the radio stores them, not guessed.** On a 2.12
  backend a legacy −1024 shows as 0 and carries the `version-trap` finding.
- **Multi-protocol names.** describe.py resolves `subType "P,S"` through the
  SD card's `Multi.txt`. Keep that file as an optional input (`--multi FILE`),
  because the protocol list lives on the SD card, not in EdgeTX.

### Acceptance

1. **Parity:** for every model on rc-config `main` and both `baseline-*`
   branches, compare `describe.py --json` with `edgetx-lint describe` field
   by field, locally only (rc-config stays read-only, nothing copied). Every
   difference must be explained, and is expected to be one of findings 1–5
   above. Record counts and categories in `docs/ACCEPTANCE.md`; no values.
2. **Synthetic fixtures** for the decoding edge cases: expo GVar weight
   (8-bit), mix offset GVar, legacy number on 2.12, unknown key, parse abort.
3. **rc-config switch-over** (separate rc-config PR, owner approval needed):
   - `build_docs.py` calls `edgetx-lint describe` (via `scripts/install.sh`
     in CI) instead of describe.py.
   - The docs diff shows only the expected corrections.
   - rc-config `CLAUDE.md` "Numbers and switch names come only from the YAML
     (via describe.py)" becomes "only from `edgetx-lint describe` (what the
     radio loads)".
   - describe.py is deleted after one release cycle.

### Tradeoffs and effort

- **Cost:** describe.py is ~420 lines of easy-to-edit Python. The port ties
  rc-config docs to an edgetx-lint release and to the pinned EdgeTX version.
  Accepted, because a wrong number in the docs of a real aircraft costs more
  than release coupling.
- **Effort:** about 1 day.
  - JSON schema plus C++ extraction using EdgeTX helpers: half a day.
  - Parity check and fixtures: a quarter day.
  - rc-config switch-over PR: a quarter day.

## rc-config CI design (`docs/rc-config/lint.yml`)

| Job | EdgeTX | Blocking | Why |
|---|---|---|---|
| tx16s | v2.10.6 | yes | what the TX16S runs (`tx16s/edgetx.sdcard.version`: 2.10) |
| pocket | v2.10.0 | yes | what the Pocket runs (`CLAUDE.md`: 2.10.0) |
| tx16s upgrade preview | v2.12.5 | no (`continue-on-error`) | shows version traps before upgrading |
| pocket upgrade preview | v2.12.5 | no | same |
| docs up to date | — | yes | `build_docs.py tx16s` + `pocket`, then `git diff --exit-code docs/` |

- Files linted: `<radio>/MODELS/*.yml <radio>/RADIO/radio.yml`.
  `labels.yml` is skipped automatically (info).
- `--quiet` hides info findings (legacy keys, normalised values) in the PR.
- **After a firmware upgrade:** change the blocking job's `edgetx` value in
  the same PR that commits the files pulled from the radio (rc-config rule:
  "upgrade: EdgeTX X.Y.Z rewrite"). Then move the preview to the next release,
  or drop it.
- If the radio runs a version without a backend, add the tag to edgetx-lint's
  CI + release matrices first (see "Adding an EdgeTX version").

Verified 2026-11-26 against rc-config `main` (`ea98a86`, TX16S models
12/13/14/16, Pocket `model00`). All four combinations give 0 errors and
0 warnings; the docs regenerate with no diff.
The earlier findings (model7 lowercase-hex parse abort; model8/9 version trap)
are gone because those models were deleted.

## Adding an EdgeTX version

1. Clone it: `git clone --depth 1 --branch vX.Y.Z --recurse-submodules
   --shallow-submodules https://github.com/EdgeTX/edgetx.git /tmp/edgetx-vX.Y.Z`
2. `scripts/build.sh tx16s /tmp/edgetx-vX.Y.Z` and the same for `pocket`.
3. Run every fixture against it, compare with the nearest version, and add a
   `"vX.Y.Z"` entry to each line of `tests/fixtures/expect.json`. Don't
   assume: 2.11.8 still rejects lowercase hex escapes, which only 2.12
   accepts.
4. `python3 tests/run_tests.py --bin-dir dist/bin --update`. The goldens for
   existing tags must not change. Then run the tests again without
   `--update`.
5. Add the tag to the matrices in `.github/workflows/ci.yml` and
   `release.yml`, and to the README.

## Session environment gotchas (Claude Code cloud)

- **Pushing:** the git proxy only accepts the session branch. Tag pushes and
  branch deletions return HTTP 403. That's why releases go through the Release
  workflow's `workflow_dispatch`, which creates the tag.
- **GitHub:** use the GitHub MCP tools. `gh` is only for `gh api search/...`
  (search/repositories shows whether a repo is private).
- **Merging:** only when the owner asks in chat. Use a merge commit.
- **rc-config:** attach with `add_repo` `access: "read"` only. Never copy its
  files into this repo. `docs/ACCEPTANCE.md` holds counts and rule names
  only. Branches `baseline-2.10.6` and `baseline-pocket-2.10.0` hold the
  original imports.
- **Build deps:** `scripts/setup-deps.sh`, or by hand: `libclang-18-dev`, a
  `/usr/local/lib/libclang.so` symlink (EdgeTX ≤2.10 needs it), and
  `pip install clang==18.1.8 pillow lz4 jinja2 asciitree pydantic`.
- **Faster:** `scripts/install.sh v0.1.1` gives all backends without
  building (after step 2).
- **Simulating GitHub shells:** test `run:` scripts with
  `bash --noprofile --norc -e -o pipefail`, not plain `bash`. PR #4's second
  bug only shows up under `-e`.
- **Checking workflow syntax:** you may parse `action.yml` and workflow
  files with PyYAML (`yaml.safe_load`). The no-generic-YAML rule applies to
  EdgeTX files only. PR #4's first bug (an unquoted `: ` in a description)
  would have been caught this way.

## Known limitations / later

- `postModelLoad()` fix-ups are not simulated.
- Version-trap ranges assume 9 GVars (true for TX16S; Pocket not verified).
- Tier 1b Companion cross-check: deferred (needs Qt6 and Rust `rs_dfu`; see the
  README).
- Out of scope: mixer behaviour tests (EdgeTX PR #7220),
  simulator screenshots (PR #7668), macOS/Windows builds.
- `actions/checkout@v4` runs on Node 20 and gets a deprecation warning.
  Dependabot (github-actions, weekly) should open the bump.
