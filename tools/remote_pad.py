"""Streams a controller to a running nhl3on3 instance started with
--remote_pad_port, where it plays as local player --slot.

Windows only. Reads an XInput controller (--pad 0-3), or the keyboard with
--keyboard, which works without focus so it can drive a slot on the same PC
while a real controller plays slot 0:

    W/A/S/D  left stick       arrow keys  D-pad
    J A   K B   U X   I Y     Q / O       left / right shoulder
    Z / M left / right trigger
    Enter Start   Tab Back

Examples:
    python tools/remote_pad.py --host 127.0.0.1 --port 42130 --slot 1 --keyboard
    python tools/remote_pad.py --host 203.0.113.5 --port 42130 --slot 1 --pad 0
"""

import argparse
import ctypes
import socket
import struct
import sys
import time

DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT = 0x0001, 0x0002, 0x0004, 0x0008
START, BACK = 0x0010, 0x0020
LEFT_SHOULDER, RIGHT_SHOULDER = 0x0100, 0x0200
A, B, X, Y = 0x1000, 0x2000, 0x4000, 0x8000

KEY_BUTTONS = {
    ord("J"): A, ord("K"): B, ord("U"): X, ord("I"): Y,
    ord("Q"): LEFT_SHOULDER, ord("O"): RIGHT_SHOULDER,
    0x26: DPAD_UP, 0x28: DPAD_DOWN, 0x25: DPAD_LEFT, 0x27: DPAD_RIGHT,
    0x0D: START, 0x09: BACK,
}


class XInputGamepad(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_ushort), ("left_trigger", ctypes.c_ubyte),
                ("right_trigger", ctypes.c_ubyte), ("lx", ctypes.c_short), ("ly", ctypes.c_short),
                ("rx", ctypes.c_short), ("ry", ctypes.c_short)]


class XInputState(ctypes.Structure):
    _fields_ = [("packet_number", ctypes.c_uint), ("gamepad", XInputGamepad)]


def load_xinput():
    for name in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
        try:
            return ctypes.WinDLL(name)
        except OSError:
            continue
    sys.exit("no XInput DLL found")


def read_pad(xinput, index):
    state = XInputState()
    if xinput.XInputGetState(index, ctypes.byref(state)) != 0:
        return None
    g = state.gamepad
    return g.buttons, g.left_trigger, g.right_trigger, g.lx, g.ly, g.rx, g.ry


def read_keyboard(user32):
    def down(vk):
        return bool(user32.GetAsyncKeyState(vk) & 0x8000)

    buttons = 0
    for vk, bit in KEY_BUTTONS.items():
        if down(vk):
            buttons |= bit
    lx = (32767 if down(ord("D")) else 0) - (32767 if down(ord("A")) else 0)
    ly = (32767 if down(ord("W")) else 0) - (32767 if down(ord("S")) else 0)
    lt = 255 if down(ord("Z")) else 0
    rt = 255 if down(ord("M")) else 0
    return buttons, lt, rt, lx, ly, 0, 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1", help="address of the PC running the game")
    ap.add_argument("--port", type=int, default=42130, help="the game's --remote_pad_port")
    ap.add_argument("--slot", type=int, default=1, choices=range(4), help="local player slot to drive")
    source = ap.add_mutually_exclusive_group()
    source.add_argument("--pad", type=int, default=0, choices=range(4), help="XInput controller to read")
    source.add_argument("--keyboard", action="store_true", help="read the keyboard instead")
    ap.add_argument("--rate", type=int, default=120, help="packets per second")
    args = ap.parse_args()

    if sys.platform != "win32":
        sys.exit("remote_pad.py reads XInput and the keyboard through Windows APIs")

    xinput = None if args.keyboard else load_xinput()
    user32 = ctypes.WinDLL("user32") if args.keyboard else None
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    source_name = "keyboard" if args.keyboard else f"XInput controller {args.pad}"
    print(f"sending {source_name} to {args.host}:{args.port} as slot {args.slot}; Ctrl+C to stop")

    sequence = 0
    connected = None
    interval = 1.0 / args.rate
    while True:
        pad = read_keyboard(user32) if args.keyboard else read_pad(xinput, args.pad)
        if (pad is not None) != connected:
            connected = pad is not None
            print("controller connected" if connected else "controller not connected; waiting")
        if pad is not None:
            sequence = (sequence + 1) & 0xFFFFFFFF
            packet = b"NPAD" + struct.pack("<B3xI", args.slot, sequence) + struct.pack("<HBBhhhh", *pad)
            sock.sendto(packet, target)
        time.sleep(interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
