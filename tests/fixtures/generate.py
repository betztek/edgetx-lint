#!/usr/bin/env python3
"""Regenerate the synthetic test fixtures.

Every fixture starts from EdgeTX's OWN writer output: the default model and
radio settings emitted by an EdgeTX 2.10.6 backend (`dump-default`). Broken
fixtures apply one small, deliberate text edit each. No YAML library is used
(they misread EdgeTX values such as ON/OFF), only exact string replacement on
the raw bytes, so CRLF line endings from the EdgeTX writer are preserved.

The fixtures are synthetic: nothing here comes from a real user's files.

usage: generate.py BACKEND_DIR   (dir holding edgetx-lint-<radio>-v2.10.6)
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASE_TAG = "v2.10.6"


def dump(backend_dir, radio, what):
    exe = os.path.join(backend_dir, f"edgetx-lint-{radio}-{BASE_TAG}")
    return subprocess.run([exe, "dump-default", what], check=True,
                          capture_output=True).stdout


def sub(data, old, new, count=1):
    old_b, new_b = old.encode(), new.encode()
    n = data.count(old_b)
    if n < count:
        raise SystemExit(f"fixture edit failed: {old!r} found {n} times")
    return data.replace(old_b, new_b, count)


def nth_sub(data, old, new, nth):
    """Replace only the nth (0-based) occurrence of old."""
    old_b = old.encode()
    pos = -1
    for _ in range(nth + 1):
        pos = data.find(old_b, pos + 1)
        if pos < 0:
            raise SystemExit(f"fixture edit failed: {old!r} occurrence {nth}")
    return data[:pos] + new.encode() + data[pos + len(old_b):]


def write(radio, sub_dir, name, data):
    d = os.path.join(HERE, radio, sub_dir)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def remove_block(data, header, next_header):
    a = data.find(header.encode())
    b = data.find(next_header.encode(), a)
    if a < 0 or b < 0:
        raise SystemExit(f"fixture edit failed: block {header!r}")
    return data[:a] + data[b:]


def models(radio, model):
    # default model name differs per board (MODEL00 / MODEL01)
    a = model.index(b'name: "MODEL')
    name_line = model[a:model.index(b'"', a + 7) + 1].decode()

    # ---------------- valid
    write(radio, "valid", "model-default.yml", model)

    m = sub(model, name_line, 'name: "Synthetic"')
    m = nth_sub(m, "weight: 100", "weight: 75", 0)            # mix 0
    m = nth_sub(m, "weight: 100", "weight: GV2", 1)           # mix 1 GVar
    m = nth_sub(m, "offset: 0", "offset: -20", 0)             # mix 0 offset
    m = nth_sub(m, 'name: ""', 'name: "Ail"', 0)
    write(radio, "valid", "model-tuned.yml", m)

    # ---------------- broken (one problem each)
    write(radio, "broken", "typo-key.yml",
          nth_sub(model, "   weight: 100", "   wieght: 100", 1))

    write(radio, "broken", "misplaced-key.yml",
          sub(model, "displayTrims: 0", "displayTrims: 0\r\ndestCh: 2"))

    write(radio, "broken", "bad-enum.yml",
          nth_sub(model, "mltpx: ADD", "mltpx: ADDD", 2))

    write(radio, "broken", "bad-number.yml",
          sub(model, "extendedLimits: 0", "extendedLimits: yes"))

    write(radio, "broken", "out-of-range.yml",
          sub(model, "extendedLimits: 0", "extendedLimits: 3"))

    write(radio, "broken", "bad-source.yml",
          nth_sub(model, "srcRaw: I1", "srcRaw: Q9", 0))

    write(radio, "broken", "comment-line.yml",
          sub(model, "displayTrims: 0", "# trims shown on main view\r\ndisplayTrims: 0"))

    write(radio, "broken", "wrong-nesting.yml",
          sub(model, name_line, name_line + '\r\n      bitmap: "plane.png"'))

    write(radio, "broken", "duplicate-key.yml",
          sub(model, "extendedLimits: 0", "extendedLimits: 0\r\nextendedLimits: 1"))

    # Version trap: a 2.9-era file (semver < 2.11) whose mix weights are
    # numbers in the legacy GVar range (-1024 = GV1, 1023 = -GV1).
    trap = sub(model, "semver: 2.10.6", "semver: 2.9.2")
    trap = nth_sub(trap, "weight: 100", "weight: -1024", 0)
    trap = nth_sub(trap, "weight: 100", "weight: 1023", 0)
    write(radio, "broken", "legacy-gvar-weight.yml", trap)

    # Reverse trap: written by 2.11+ as a literal -1024, read by 2.10 as GV1.
    rev = sub(model, "semver: 2.10.6", "semver: 2.12.5")
    rev = nth_sub(rev, "weight: 100", "weight: -1024", 0)
    write(radio, "broken", "literal-weight-from-2.12.yml", rev)

    # EdgeTX <= 2.10 only accepts uppercase hex digits in \x escapes and
    # stops reading the file at the first lowercase one.
    write(radio, "broken", "lowercase-hex-escape.yml",
          sub(model, name_line, 'name: "Model\\x1d"'))


def radios(radio, rad):
    rad = sub(rad, "manuallyEdited: 0", "manuallyEdited: 0")  # sanity
    os.makedirs(os.path.join(HERE, radio, "valid", "RADIO"), exist_ok=True)
    os.makedirs(os.path.join(HERE, radio, "broken", "RADIO-checksum"), exist_ok=True)
    os.makedirs(os.path.join(HERE, radio, "broken", "RADIO-typo"), exist_ok=True)

    if b"flexSwitches:" in rad:
        # 2.10 writes inactive flex switch channels that 2.12 drops; keep the
        # valid fixture valid for both versions.
        rad = remove_block(rad, "flexSwitches:", "currModelFilename:")
    write(radio, "valid/RADIO", "radio.yml", rad)

    # A hand edit without fixing the checksum and without manuallyEdited
    bad = b"checksum: 12345\r\n" + sub(rad, "ppmunit: 0", "ppmunit: 1")
    write(radio, "broken/RADIO-checksum", "radio.yml", bad)

    write(radio, "broken/RADIO-typo", "radio.yml",
          sub(rad, "ppmunit: 0", "ppmUnit: 0"))


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    backend_dir = sys.argv[1]
    for radio in ("tx16s", "pocket"):
        models(radio, dump(backend_dir, radio, "model"))
        radios(radio, dump(backend_dir, radio, "radio"))


if __name__ == "__main__":
    main()
