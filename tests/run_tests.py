#!/usr/bin/env python3
"""edgetx-lint test runner.

For every installed backend (radio x EdgeTX tag) it lints every fixture under
tests/fixtures/<radio>/ and checks:
  1. valid/ fixtures: no errors, no warnings, exit 0
  2. broken/ fixtures: error+warning rule ids == tests/fixtures/expect.json,
     and every broken fixture is caught on at least one tag
  3. text output == golden file tests/expected/<tag>/<radio>/<fixture>.txt
  4. CLI contract: exit codes, --warn-unknown, --format json/github, dispatcher
  5. describe: for tests/fixtures/<radio>/describe/*.yml (+ valid/model-default.yml)
     `describe valid/RADIO/radio.yml FILE` == golden
     tests/expected/<tag>/<radio>/describe/<name>.json (tool version masked),
     exit 1 iff the embedded lint has errors, plus semantic checks per fixture.
     describe/ fixtures are inputs for these tests only; the check loop skips them.

usage: run_tests.py --bin-dir DIR [--update]
  DIR holds the `edgetx-lint` dispatcher and/or edgetx-lint-<radio>-<tag>
  backends (a dist/ tree or a flat dir of symlinks both work).

No YAML library is used anywhere: outputs are compared as text, and the JSON
format is parsed with the stdlib json module.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")
EXP = os.path.join(ROOT, "tests", "expected")

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL:", msg)


def find_backends(bin_dir):
    found = {}
    for d in (bin_dir, os.path.join(bin_dir, "..", "libexec", "edgetx-lint"),
              os.path.join(bin_dir, "libexec", "edgetx-lint")):
        for p in glob.glob(os.path.join(d, "edgetx-lint-*-v*")):
            m = re.match(r"edgetx-lint-([a-z0-9]+)-(v[0-9.]+)$", os.path.basename(p))
            if m and os.access(p, os.X_OK):
                found.setdefault((m.group(1), m.group(2)), os.path.abspath(p))
    return found


def run(cmd):
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def fixtures(radio):
    files = glob.glob(os.path.join(FIX, radio, "**", "*.yml"), recursive=True)
    return sorted(os.path.relpath(f, ROOT) for f in files)


def rules_of(json_out):
    doc = json.loads(json_out)
    rules = set()
    for f in doc["files"]:
        for x in f["findings"]:
            if x["severity"] in ("error", "warning"):
                rules.add(x["rule"])
    return rules, doc["summary"]


def check_fixtures(radio, tag, exe, expect, update, caught):
    for rel in fixtures(radio):
        sub = os.path.relpath(rel, os.path.join("tests", "fixtures", radio))
        kind = sub.split(os.sep)[0]
        if kind == "describe":
            continue  # describe inputs: see check_describe()

        rc, out, err = run([exe, "check", "--format", "json", rel])
        if rc == 2:
            fail(f"{radio} {tag} {sub}: internal/usage error: {err.strip()}")
            continue
        rules, summary = rules_of(out)

        if kind == "valid":
            if rules or rc != 0:
                fail(f"{radio} {tag} {sub}: valid fixture not clean: {sorted(rules)} rc={rc}")
        else:
            want = expect.get(sub, {}).get(tag)
            if want is None:
                fail(f"{radio} {tag} {sub}: no entry in expect.json")
            elif set(want) != rules:
                fail(f"{radio} {tag} {sub}: rules {sorted(rules)} != expected {sorted(want)}")
            if rules:
                caught.add((radio, sub))
            want_rc = 1 if summary["errors"] else 0
            if rc != want_rc:
                fail(f"{radio} {tag} {sub}: exit {rc}, expected {want_rc}")

        # golden text output
        rc, out, _ = run([exe, "check", rel])
        golden = os.path.join(EXP, tag, radio, sub + ".txt")
        if update:
            os.makedirs(os.path.dirname(golden), exist_ok=True)
            with open(golden, "w", newline="") as f:
                f.write(out)
        elif not os.path.exists(golden):
            fail(f"{radio} {tag} {sub}: missing golden {os.path.relpath(golden, ROOT)}")
        else:
            with open(golden, newline="") as f:
                if f.read() != out:
                    fail(f"{radio} {tag} {sub}: output differs from golden "
                         f"{os.path.relpath(golden, ROOT)} (run with --update to see)")


def check_cli(radio, tag, exe):
    typo = f"tests/fixtures/{radio}/broken/typo-key.yml"
    valid = f"tests/fixtures/{radio}/valid/model-default.yml"

    rc, out, _ = run([exe, "check", "--warn-unknown", typo])
    if rc != 0 or "warning [unknown-key]" not in out:
        fail(f"{radio} {tag}: --warn-unknown should downgrade to warning and exit 0 (rc={rc})")

    rc, _, _ = run([exe, "check", "--warn-unknown", "--strict", typo])
    if rc != 1:
        fail(f"{radio} {tag}: --strict should fail on warnings (rc={rc})")

    rc, out, _ = run([exe, "check", "--format", "github", typo])
    if rc != 1 or not out.startswith("::error file="):
        fail(f"{radio} {tag}: --format github output/exit wrong (rc={rc})")

    rc, out, _ = run([exe, "check", valid])
    if rc != 0:
        fail(f"{radio} {tag}: valid file exit {rc}")

    rc, _, _ = run([exe, "check", "--bogus", valid])
    if rc != 2:
        fail(f"{radio} {tag}: unknown option should exit 2 (rc={rc})")

    rc, _, _ = run([exe, "check", "--radio", "nope", valid])
    if rc != 2:
        fail(f"{radio} {tag}: wrong --radio should exit 2 (rc={rc})")

    rc, out, _ = run([exe, "check", "--format", "json", "tests/does-not-exist.yml"])
    if rc != 1 or json.loads(out)["files"][0]["findings"][0]["rule"] != "io-error":
        fail(f"{radio} {tag}: missing file should be an io-error finding (rc={rc})")


def tag_tuple(tag):
    return tuple(int(x) for x in tag.lstrip("v").split("."))


TOOL_VERSION = re.compile(r'("tool": \{\n    "name": "edgetx-lint",\n    "version": )"[^"]*"')


def describe_fixtures(radio):
    files = sorted(glob.glob(os.path.join(FIX, radio, "describe", "*.yml")))
    files.append(os.path.join(FIX, radio, "valid", "model-default.yml"))
    return [os.path.relpath(f, ROOT) for f in files]


def lint_rules(model, severity=None):
    return {f["rule"] for f in model["lint"]["findings"]
            if severity is None or f["severity"] == severity}


def describe_semantics(radio, tag, name, doc):
    """Key edge cases, read from the JSON (not just golden diffs)."""
    where = f"{radio} {tag} describe/{name}"
    model = doc["models"][0]
    old = tag_tuple(tag) < (2, 11)

    def check(cond, msg):
        if not cond:
            fail(f"{where}: {msg}")

    if name in ("rich", "model-default"):
        check(model["lint"]["errors"] == 0 and model["lint"]["warnings"] == 0,
              f"expected clean lint, got {sorted(lint_rules(model))}")
        check(doc["radio"] and doc["radio"]["lint"]["errors"] == 0, "radio.yml not clean")
    if name == "expo-gvar-weight":
        w = model["inputs"][0]["weight"]
        check(w.get("gvar") == 3 and not w.get("negated"), f"input weight not GV3: {w}")
    elif name == "mix-gvar-offset":
        o = model["mixes"][0]["offset"]
        check(o.get("gvar") == 2 and o.get("negated") is True, f"mix offset not -GV2: {o}")
    elif name == "legacy-number-2.12":
        w = model["mixes"][0]["weight"]
        if old:
            check(w.get("text") == "GV1" and w.get("gvar") == 1, f"weight not GV1: {w}")
            check("version-trap" not in lint_rules(model), "unexpected version-trap")
        else:
            check(w.get("value") == 0 and "gvar" not in w, f"weight not stored as 0: {w}")
            check("version-trap" in lint_rules(model), "missing version-trap finding")
    elif name == "unknown-key":
        facts = json.dumps({k: v for k, v in model.items() if k != "lint"})
        check("expoWeight" not in facts and "37" not in facts,
              "ignored key/value leaked into describe output")
        check("unknown-key" in lint_rules(model, "error"), "missing unknown-key finding")
        check(len(model["mixes"]) == 4, "mixes lost")
    elif name == "parse-abort":
        aborts = tag_tuple(tag) < (2, 12)
        check(("parse-aborted" in lint_rules(model)) == aborts,
              f"parse-aborted finding {'missing' if aborts else 'unexpected'}")
        lines = [x["line"] for x in model.get("mixes", [])]
        if aborts:
            check(not [ln for ln in lines if ln >= 3],
                  f"mixes after the abort line still described: {lines}")
        else:
            check(lines == [1, 2, 3, 4], f"mixes {lines}")
    elif name == "rich":
        check(model["name"] == "Rich", f"name {model['name']!r}")
        t = model["timers"]
        check(len(t) == 1 and t[0]["name"] == "Flt" and t[0]["switch"]["raw"] == "SA2"
              and t[0]["start_s"] == 300, f"timers {t}")
        fm = [f for f in model["flight_modes"] if f["index"] == 1]
        check(fm and fm[0]["name"] == "Launch" and fm[0]["switch"]["raw"] == "SB0",
              f"flight modes {model['flight_modes']}")
        mx = model["mixes"][0]
        check(mx["name"] == "Ail" and mx["switch"]["raw"] == "SC2"
              and 1 not in mx["flight_modes"]["active_in"]
              and 0 in mx["flight_modes"]["active_in"], f"mix 1 {mx}")
        check(model["inputs"][0]["name"] == "Low", "input name")
        out = model["outputs"][0]
        check(out["name"] == "Ail" and out["reversed"] is True
              and out["min"].get("value") == -90 and out["max"].get("value") == 95,
              f"output 1 {out}")
        check(len(model["logical_switches"]) == 1
              and model["logical_switches"][0]["func"] == "FUNC_VPOS", "logical switches")
        check(len(model["special_functions"]) == 1
              and model["special_functions"][0]["switch"]["raw"] == "SA2", "special functions")
        gv = model["global_variables"]
        check(len(gv) == 1 and gv[0]["index"] == 3 and gv[0]["name"] == "Rte"
              and gv[0]["values"][0]["value"] == 30, f"gvars {gv}")


def check_describe(radio, tag, exe, update):
    rad = f"tests/fixtures/{radio}/valid/RADIO/radio.yml"
    for rel in describe_fixtures(radio):
        name = os.path.splitext(os.path.basename(rel))[0]
        where = f"{radio} {tag} describe/{name}"
        rc, out, err = run([exe, "describe", rad, rel])
        try:
            doc = json.loads(out)
        except ValueError as e:
            fail(f"{where}: output is not JSON ({e}); rc={rc} {err.strip()}")
            continue
        if doc.get("schema") != 1:
            fail(f"{where}: schema {doc.get('schema')!r} != 1")
        errors = sum(x["lint"]["errors"] for x in [doc["radio"]] + doc["models"] if x)
        if rc != (1 if errors else 0):
            fail(f"{where}: exit {rc}, expected {1 if errors else 0} ({errors} lint errors)")
        describe_semantics(radio, tag, name, doc)

        # golden (the tool version is masked so a release bump doesn't touch them)
        out = TOOL_VERSION.sub(r'\1"VERSION"', out, count=1)
        golden = os.path.join(EXP, tag, radio, "describe", name + ".json")
        if update:
            os.makedirs(os.path.dirname(golden), exist_ok=True)
            with open(golden, "w", newline="") as f:
                f.write(out)
        elif not os.path.exists(golden):
            fail(f"{where}: missing golden {os.path.relpath(golden, ROOT)}")
        else:
            with open(golden, newline="") as f:
                if f.read() != out:
                    fail(f"{where}: output differs from golden "
                         f"{os.path.relpath(golden, ROOT)} (run with --update to see)")

    rc, _, _ = run([exe, "describe"])
    if rc != 2:
        fail(f"{radio} {tag}: describe without files should exit 2 (rc={rc})")


def check_dispatcher(bin_dir, backends):
    disp = os.path.join(bin_dir, "edgetx-lint")
    if not os.access(disp, os.X_OK):
        print("note: no dispatcher in", bin_dir, "- skipping dispatcher tests")
        return
    rc, out, _ = run([disp, "list"])
    for (radio, tag) in backends:
        if f"{radio}" not in out or tag not in out:
            fail(f"dispatcher list misses {radio} {tag}")
    for (radio, tag) in backends:
        valid = f"tests/fixtures/{radio}/valid/model-default.yml"
        rc, out, err = run([disp, "check", "--radio", radio, "--edgetx", tag, valid])
        if rc != 0 or f"EdgeTX {tag}" not in out:
            fail(f"dispatcher {radio} {tag}: rc={rc} {err.strip()}")
    rc, _, _ = run([disp, "check", "--radio", "nosuchradio", "x.yml"])
    if rc != 2:
        fail(f"dispatcher: unknown radio should exit 2 (rc={rc})")
    rc, _, _ = run([disp, "check", "x.yml"])
    if rc != 2:
        fail(f"dispatcher: missing --radio should exit 2 (rc={rc})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", required=True)
    ap.add_argument("--update", action="store_true", help="rewrite golden files")
    args = ap.parse_args()

    with open(os.path.join(FIX, "expect.json")) as f:
        expect = {k: v for k, v in json.load(f).items() if not k.startswith("_")}

    backends = find_backends(args.bin_dir)
    if not backends:
        print("no backends found in", args.bin_dir)
        return 2

    caught = set()
    for (radio, tag), exe in sorted(backends.items()):
        print(f"== {radio} {tag}")
        check_fixtures(radio, tag, exe, expect, args.update, caught)
        check_cli(radio, tag, exe)
        check_describe(radio, tag, exe, args.update)

    # Every broken fixture must be expected to fail on at least one tag;
    # check_fixtures() enforces the expectation on each tag that is tested.
    for radio in sorted({r for r, _ in backends}):
        for rel in fixtures(radio):
            sub = os.path.relpath(rel, os.path.join("tests", "fixtures", radio))
            if sub.startswith("broken") and not any(expect.get(sub, {}).values()):
                fail(f"{radio} {sub}: broken fixture has no expected findings on any tag")

    check_dispatcher(args.bin_dir, backends)

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
