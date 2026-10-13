#!/usr/bin/env python3
"""Send key presses to the nhl3on3 window under X11 (WSLg) via XTEST.

Usage: xkey.py KEY[:HOLD_MS] [KEY ...]   e.g. xkey.py Return Down Down Return
A bare number N sleeps N milliseconds between keys. Needs python-xlib and the
game started with SDL_VIDEODRIVER=x11 --mnk_mode=true.
"""
import sys
import time

from Xlib import X, XK, display
from Xlib.ext import xtest


def find_window(node, needle):
    name = node.get_wm_name()
    if isinstance(name, str) and needle in name:
        return node
    for child in node.query_tree().children:
        found = find_window(child, needle)
        if found:
            return found
    return None


def main():
    d = display.Display()
    win = find_window(d.screen().root, "nhl3on3 [")
    if not win:
        sys.exit("nhl3on3 window not found")
    win.set_input_focus(X.RevertToParent, X.CurrentTime)
    win.raise_window()
    d.sync()
    time.sleep(0.2)
    for arg in sys.argv[1:]:
        if arg.isdigit():
            time.sleep(int(arg) / 1000)
            continue
        key, _, hold = arg.partition(":")
        code = d.keysym_to_keycode(XK.string_to_keysym(key))
        if not code:
            sys.exit(f"unknown key {key}")
        xtest.fake_input(d, X.KeyPress, code)
        d.sync()
        time.sleep(int(hold or 120) / 1000)
        xtest.fake_input(d, X.KeyRelease, code)
        d.sync()
        time.sleep(0.35)


if __name__ == "__main__":
    main()
