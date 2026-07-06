#!/usr/bin/env python3
"""Seed a libFuzzer corpus from the recorded RS-485 capture fixtures.

The capture fixtures under test/fixtures/*.cap are line-oriented text recordings
of the serial bus (see the header comment inside each .cap):

    [timestamp_ms] <direction> 0xNN|0xNN|0xNN|...

where <direction> is R (read from device) or W (write to device).  Each such line
is one on-the-wire frame.  This script extracts every frame, writes each as a raw
binary file, and sorts them into per-protocol corpus directories so each fuzz
harness starts from real, valid traffic:

    fuzz/corpus/jandy/     -- Jandy DLE/STX framed frames
    fuzz/corpus/pentair/   -- Pentair 0xFF 0x00 0xFF 0xA5 framed frames

A Pentair frame is identified by the 0xFF 0x00 0xFF 0xA5 preamble; everything else
is treated as Jandy.  Filenames are the SHA-1 of the frame bytes (libFuzzer's own
corpus-file naming convention), so re-running is idempotent and de-duplicates
identical frames across fixtures.

Usage:
    python3 fuzz/seed_corpus.py                 # default in/out paths
    python3 fuzz/seed_corpus.py --fixtures DIR --out DIR
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys

# A data line looks like: [   142] R 0x10|0x02|0x50|0x14|0x76|0x10|0x03
_LINE_RE = re.compile(r"^\s*\[\s*\d+\s*\]\s+([RW])\s+(.+)$")
_BYTE_RE = re.compile(r"0x([0-9a-fA-F]{1,2})")

_PENTAIR_PREAMBLE = bytes([0xFF, 0x00, 0xFF, 0xA5])


def parse_frames(cap_text: str) -> list[bytes]:
    """Extract every R/W frame from one .cap file's text."""
    frames: list[bytes] = []
    for line in cap_text.splitlines():
        m = _LINE_RE.match(line)
        if not m:
            continue  # comment / metadata / blank line
        payload = m.group(2)
        byte_values = [int(h, 16) for h in _BYTE_RE.findall(payload)]
        if byte_values:
            frames.append(bytes(byte_values))
    return frames


def classify(frame: bytes) -> str:
    return "pentair" if _PENTAIR_PREAMBLE in frame else "jandy"


# Representative valid request bodies for the schedule-JSON validators (FromJson /
# ControllerScheduleFromJson). These are not derived from the .cap captures — they
# give the schedule-json harness real structure to mutate from. Kept as compact JSON
# strings (both app-schedule and controller-schedule shapes).
_SCHEDULE_JSON_SEEDS = [
    '{"name":"Morning pump","enabled":true,"days_of_week":31,"time_local":"08:05",'
    '"action":{"type":"button_on","target":"Pool Pump"}}',
    '{"days_of_week":64,"time_local":"17:30","action":{"type":"spa_setpoint","value":38}}',
    '{"days_of_week":127,"time_local":"09:00","action":{"type":"chlorinator_percent","value":60}}',
    '{"days_of_week":1,"time_local":"06:00","action":{"type":"circulation_mode","target":"Pool"}}',
    '{"uuid":"abc-123","name":"Off","enabled":false,"days_of_week":0,"time_local":"23:59",'
    '"action":{"type":"button_toggle","target":"Spa"}}',
    # Controller-schedule shape (on_local/off_local/target/group).
    '{"target":"Filter Pump","group":"A","days_of_week":62,"on_local":"07:00","off_local":"19:00"}',
    '{"target":"Aux1","days_of_week":127,"on_local":"00:00","off_local":"23:59","name":"All day"}',
]


def seed_schedule_json(out_dir: pathlib.Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for body in _SCHEDULE_JSON_SEEDS:
        data = body.encode("utf-8")
        dest = out_dir / hashlib.sha1(data).hexdigest()
        if not dest.exists():
            dest.write_bytes(data)
            written += 1
    return written


def main(argv: list[str]) -> int:
    here = pathlib.Path(__file__).resolve().parent
    repo_root = here.parent

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixtures", type=pathlib.Path, default=repo_root / "test" / "fixtures",
                        help="directory tree containing *.cap fixtures (default: test/fixtures)")
    parser.add_argument("--out", type=pathlib.Path, default=here / "corpus",
                        help="output corpus root (default: fuzz/corpus)")
    args = parser.parse_args(argv)

    cap_files = sorted(args.fixtures.rglob("*.cap"))
    if not cap_files:
        print(f"no .cap fixtures found under {args.fixtures}", file=sys.stderr)
        return 1

    written = {"jandy": 0, "pentair": 0}
    for out_dir in (args.out / "jandy", args.out / "pentair"):
        out_dir.mkdir(parents=True, exist_ok=True)

    for cap in cap_files:
        for frame in parse_frames(cap.read_text(encoding="utf-8", errors="replace")):
            protocol = classify(frame)
            name = hashlib.sha1(frame).hexdigest()
            dest = args.out / protocol / name
            if not dest.exists():
                dest.write_bytes(frame)
                written[protocol] += 1

    schedule_written = seed_schedule_json(args.out / "schedule-json")

    print(f"Seeded corpus from {len(cap_files)} fixture(s):")
    print(f"  jandy:         {written['jandy']} new frame(s) -> {args.out / 'jandy'}")
    print(f"  pentair:       {written['pentair']} new frame(s) -> {args.out / 'pentair'}")
    print(f"  schedule-json: {schedule_written} new body(ies) -> {args.out / 'schedule-json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
