#!/usr/bin/env python3
"""edgetx-lint test runner.

For every installed backend (radio x EdgeTX tag) it lints every fixture under
tests/fixtures/<radio>/ and checks:
  1. valid/ fixtures: no errors, no warnings, exit 0
  2. broken/ fixtures: error+warning rule ids == tests/fixtures/expect.json,
     and every broken fixture is caught on at least one tag
  3. text output == golden file tests/expected/<tag>/<radio>/<fixture>.txt
  4. CLI contract: exit codes, --warn-unknown, --format json/github, dispatcher

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

    for radio in sorted({r for r, _ in backends}):
        for rel in fixtures(radio):
            sub = os.path.relpath(rel, os.path.join("tests", "fixtures", radio))
            if sub.startswith("broken") and (radio, sub) not in caught:
                fail(f"{radio} {sub}: broken fixture not caught by any tested tag")

    check_dispatcher(args.bin_dir, backends)

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
