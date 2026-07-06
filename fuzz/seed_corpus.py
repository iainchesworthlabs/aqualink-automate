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


# Representative inbound WebSocket message envelopes ({type, payload}) for the
# fuzz-websocket-json harness (type names are WebSocket_EventTypes enumerators).
_WEBSOCKET_JSON_SEEDS = [
    '{"type":"TemperatureUpdate","payload":{"pool":26.5,"spa":38}}',
    '{"type":"ButtonStateChange","payload":{"id":3,"state":"on"}}',
    '{"type":"ChemistryUpdate","payload":{"ph":7.4,"orp":650}}',
    '{"type":"CirculationUpdate","payload":{"mode":"Pool"}}',
    '{"type":"SystemStatusChange","payload":{}}',
    '{"type":"unknown","payload":null}',
]

# Representative MQTT command payloads for the fuzz-mqtt-payload harness (JSON
# number, quoted number, {"raw": ...} envelope, plain string, object).
_MQTT_PAYLOAD_SEEDS = [
    "50", "101", "-1", "3.14", '"75"', '{"raw":"60"}', '"boost"', '{"raw":"on"}', "true",
]

# Representative INI config-file bodies for the fuzz-config-parse harness (keys are
# option long-names; values exercise the custom validators).
_CONFIG_PARSE_SEEDS = [
    "loglevel-main = debug\nprofiler = tracy\nverbose = true\n",
    "mqtt-protocol-version = 5.0\nlog-syslog-facility = local0\n",
    "# a comment\nname = pool\ncount = 42\n",
    "loglevel-main=trace\nmqtt-protocol-version=v3.1.1\n",
    "log-syslog-facility = daemon\n",
]

# Representative HTTP request targets for the fuzz-query-string harness.
_QUERY_STRING_SEEDS = [
    "/api/equipment/button?id=3",
    "/api/schedules?foo=bar&id=7",
    "/?id=42", "/api/controller/schedules", "/a?%zz", "//",
]

# Representative bearer tokens (JWT-shaped + garbage) for the fuzz-jwt harness.
_JWT_SEEDS = [
    "eyJhbGciOiJIUzI1NiIsImtpZCI6ImsxIn0.eyJzdWIiOiJhIn0.c2ln",
    "a.b.c", "not-a-jwt", "", "eyJ9.eyJ9.",
]

# Representative HH:MM:SS values for the fuzz-duration harness.
_DURATION_SEEDS = [
    "01:02:03", "23:59:59", "00:00:00", "99:00:00", "1:2:3", "aa:bb:cc", "12:34",
]

# Representative .cap capture-file bodies for the fuzz-replay-line harness (both the
# recorder "[ts] DIR ..." and legacy bare formats, plus comments/malformed tokens).
_REPLAY_LINE_SEEDS = [
    "[100] R 0x10|0x02|0x50|0x14|0x76|0x10|0x03\n",
    "0x10|0x02|0x00|0x01\n",
    "# comment header\n[5] W 0xff|0x00\n[6] R 0x48\n",
    "[x] R 0xZZ|0x10\n",
    "not a valid line at all\n",
]


def _seed_text(out_dir: pathlib.Path, seeds: list) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for body in seeds:
        data = body.encode("utf-8")
        dest = out_dir / hashlib.sha1(data).hexdigest()
        if not dest.exists():
            dest.write_bytes(data)
            written += 1
    return written


def seed_schedule_json(out_dir: pathlib.Path) -> int:
    return _seed_text(out_dir, _SCHEDULE_JSON_SEEDS)


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

    text_corpora = {
        "schedule-json": _SCHEDULE_JSON_SEEDS,
        "websocket-json": _WEBSOCKET_JSON_SEEDS,
        "mqtt-payload": _MQTT_PAYLOAD_SEEDS,
        "config-parse": _CONFIG_PARSE_SEEDS,
        "query-string": _QUERY_STRING_SEEDS,
        "jwt": _JWT_SEEDS,
        "duration": _DURATION_SEEDS,
        "replay-line": _REPLAY_LINE_SEEDS,
    }
    text_written = {name: _seed_text(args.out / name, seeds) for name, seeds in text_corpora.items()}

    print(f"Seeded corpus from {len(cap_files)} fixture(s):")
    print(f"  jandy:          {written['jandy']} new frame(s) -> {args.out / 'jandy'}")
    print(f"  pentair:        {written['pentair']} new frame(s) -> {args.out / 'pentair'}")
    for name, count in text_written.items():
        print(f"  {name + ':':15s} {count} new seed(s) -> {args.out / name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
