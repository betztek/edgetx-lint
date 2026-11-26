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
- Out of scope: `describe` port, mixer behaviour tests (EdgeTX PR #7220),
  simulator screenshots (PR #7668), macOS/Windows builds.
- `actions/checkout@v4` runs on Node 20 and gets a deprecation warning.
  Dependabot (github-actions, weekly) should open the bump.
