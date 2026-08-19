#!/usr/bin/env python3
"""Evaluate the W6 forced-termination recovery trace.

This T4b probe closes only the instrument-discrimination requirement.  W6 owns
the product runner, the real post-ack process termination, restart/recovery
selection, and the final zero-loss gate.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class TraceError(ValueError):
    """The trace cannot demonstrate the frozen loss contract."""


@dataclass(frozen=True)
class InputEvent:
    seq: int
    text: str


@dataclass(frozen=True)
class Verdict:
    expected_text: str
    recovered_text: str
    recovered_events: int
    lost_events: int


def _require_bool(document: dict[str, Any], key: str) -> bool:
    value = document.get(key)
    if type(value) is not bool:
        raise TraceError(f"{key} must be a boolean")
    return value


def _require_int(document: dict[str, Any], key: str) -> int:
    value = document.get(key)
    if type(value) is not int:
        raise TraceError(f"{key} must be an integer")
    return value


def evaluate_trace(document: dict[str, Any]) -> Verdict:
    """Validate one trace and return its event-boundary loss result."""

    if document.get("schema") != "pynote.w6-loss-trace.v1":
        raise TraceError("schema must be pynote.w6-loss-trace.v1")

    maximum_loss = _require_int(document, "max_loss_keystrokes")
    if maximum_loss != 0:
        raise TraceError("the frozen post-ack contract requires max_loss_keystrokes=0")

    raw_events = document.get("events")
    if not isinstance(raw_events, list) or not raw_events:
        raise TraceError("events must be a non-empty array")

    events: list[InputEvent] = []
    for index, raw_event in enumerate(raw_events, start=1):
        if not isinstance(raw_event, dict):
            raise TraceError(f"events[{index - 1}] must be an object")
        seq = raw_event.get("seq")
        text = raw_event.get("text")
        if type(seq) is not int or seq != index:
            raise TraceError("event seq values must be consecutive integers starting at 1")
        if not isinstance(text, str) or not text:
            raise TraceError(f"event {seq} text must be a non-empty string")
        events.append(InputEvent(seq=seq, text=text))

    kill_after_seq = _require_int(document, "kill_after_seq")
    if kill_after_seq != events[-1].seq:
        raise TraceError("kill_after_seq must identify the final input event before termination")

    durable_ack_seq = _require_int(document, "durable_ack_seq")
    if durable_ack_seq < kill_after_seq:
        raise TraceError("forced termination occurred before protection acknowledged the kill point")
    if not _require_bool(document, "forced_termination_after_ack"):
        raise TraceError("forced_termination_after_ack must be true")
    if not _require_bool(document, "restart_launched"):
        raise TraceError("restart_launched must be true")
    if not _require_bool(document, "recovery_selected"):
        raise TraceError("recovery_selected must be true")

    recovered = document.get("recovered_text")
    if not isinstance(recovered, str):
        raise TraceError("recovered_text must be a string")

    expected = "".join(event.text for event in events)
    boundaries = [""]
    for event in events:
        boundaries.append(boundaries[-1] + event.text)

    if recovered not in boundaries:
        if expected.startswith(recovered):
            raise TraceError("recovered_text ends inside an input event")
        raise TraceError("recovered_text diverges, reorders, or contains extra text")

    recovered_events = boundaries.index(recovered)
    lost_events = len(events) - recovered_events
    if lost_events > maximum_loss:
        raise TraceError(
            f"lost_events={lost_events} exceeds max_loss_keystrokes={maximum_loss}"
        )

    return Verdict(
        expected_text=expected,
        recovered_text=recovered,
        recovered_events=recovered_events,
        lost_events=lost_events,
    )


def _known_good() -> dict[str, Any]:
    return {
        "schema": "pynote.w6-loss-trace.v1",
        "max_loss_keystrokes": 0,
        "events": [
            {"seq": 1, "text": "a"},
            {"seq": 2, "text": "한글"},
            {"seq": 3, "text": "🙂"},
        ],
        "kill_after_seq": 3,
        "durable_ack_seq": 3,
        "forced_termination_after_ack": True,
        "restart_launched": True,
        "recovery_selected": True,
        "recovered_text": "a한글🙂",
    }


def run_self_test() -> int:
    good = _known_good()
    verdict = evaluate_trace(good)
    if verdict.lost_events != 0:
        print("FAIL: known-good did not produce zero loss")
        return 1

    seeded_bad = []
    lost_event = dict(good)
    lost_event["recovered_text"] = "a한글"
    seeded_bad.append(("one-event loss", lost_event))

    partial_ime = dict(good)
    partial_ime["recovered_text"] = "a한"
    seeded_bad.append(("partial IME event", partial_ime))

    for label, trace in seeded_bad:
        try:
            evaluate_trace(trace)
        except TraceError as exc:
            print(f"PASS seeded-bad [{label}]: {exc}")
        else:
            print(f"FAIL: seeded-bad [{label}] was accepted")
            return 1

    print("PASS known-good: post-ack restart/recovery, lost_events=0")
    print("PASS loss-probe self-test: known-good accepted; seeded bad rejected")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--self-test", action="store_true")
    group.add_argument("--trace", type=Path)
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()

    try:
        document = json.loads(args.trace.read_text(encoding="utf-8"))
        if not isinstance(document, dict):
            raise TraceError("trace root must be an object")
        verdict = evaluate_trace(document)
    except (OSError, UnicodeError, json.JSONDecodeError, TraceError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print(
        json.dumps(
            {
                "verdict": "PASS",
                "recovered_events": verdict.recovered_events,
                "lost_events": verdict.lost_events,
                "expected_text": verdict.expected_text,
                "recovered_text": verdict.recovered_text,
            },
            ensure_ascii=False,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
