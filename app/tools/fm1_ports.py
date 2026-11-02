#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""Which MIDI port is the FM-1, and what is everything else?

`fm1_resources.py` prints `device: FM-1_9400  (FM-1 CTRL)`: the identity on the left
comes from the device answering the updater's handshake, the parenthesised name is
the MIDI INPUT port that did the answering. When the name is not what is plugged in,
this lists every port, asks each one for the identity, and says which one the tools
would pick -- so "who answered" stops being a guess.

    python3 platform/tools/fm1_ports.py

Needs mido with python-rtmidi (the same as the other tools). Nothing is sent except
the handshake query, which every fm1-base firmware answers and every other device
ignores.
"""
import os
import sys

_TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "..", "platform", "tools")
sys.path.insert(0, os.path.normpath(_TOOLS))

import fm1_install as I  # noqa: E402
import fm1ports  # noqa: E402


def main():
    try:
        mido = I.MidoBackend()
    except ImportError:
        raise SystemExit("needs mido and python-rtmidi (pip install mido python-rtmidi)")

    ins = mido.input_names()
    outs = mido.output_names()

    print(f"inputs ({len(ins)}):")
    for n in ins:
        usual = "  <- an FM-1 name, tried first" if fm1ports.PORT_RE.search(n) else ""
        print(f"  {n}{usual}")
    print(f"\noutputs ({len(outs)}):")
    for n in outs:
        print(f"  {n}")

    if not ins:
        raise SystemExit("\nno MIDI inputs at all: is the FM-1 plugged in?")

    print("\nhandshake (cmd 0x11: the identity each port's device answers):")
    said = {}
    for name in ins:
        out = fm1ports.pair_output(name, outs)
        if not out:
            print(f"  {name}\n      no output to send the query to: skipped")
            continue
        try:
            link = mido.open(name, out)
        except Exception as e:  # noqa: BLE001 (whatever mido raises: this is a report)
            print(f"  {name}\n      cannot open: {e}")
            continue
        try:
            ident = I.handshake(link, 2)
        finally:
            link.close()
        said[name] = ident
        if ident:
            print(f"  {name}\n      identity {ident.text}   (out: {out})")
        else:
            print(f"  {name}\n      no answer (not an fm1-base firmware)")

    answering = [n for n, v in said.items() if v]
    print()
    if not answering:
        raise SystemExit("nothing answered the handshake: the FM-1 is not on a MIDI port "
                         "(an FM-1 in update mode does not either -- plug the cable out and in)")
    if len(answering) == 1:
        print(f"the tools will use: {answering[0]}")
    else:
        print("MORE THAN ONE PORT ANSWERED. fm1_resources.py takes the first candidate, so "
              "which one it uses is not obvious:")
        for n in answering:
            print(f"  {n}  ({said[n].text})")
        print("\npass --port <part of the name> to pick one.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
