#!/usr/bin/env python3
"""Builds the team play front-end mod: patches ActionScript in the game's Apt
screens and writes the result back into data0.big / data0.bh.

Usage: ui_mod.py GAME_DIR [--restore] [--check] [--emit-cpp [PATH]]

GAME_DIR is the extracted game (with data0.big). The first run copies the
originals to data0.big.orig / data0.bh.orig; every run rebuilds from those, so
it is safe to re-run. --restore puts the originals back. --check only patches
in memory and prints the disassembly of every patched function.

--emit-cpp writes the patches to src/nhl3on3_ui_patches.inc, from which the
game patches data0.big itself on startup (src/nhl3on3_ui_patch.cpp), so
players never run this script. Re-run it after changing a patch.

Patched screens are appended to data0.big uncompressed and both directories
(the .big header and the .bh index) point at the new copy.

Patches are written in place over code they make dead, so the Apt files keep
their size and layout; see PATCHES below.
"""
import argparse
import io
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apt_dis  # noqa: E402
import ea_big  # noqa: E402

# --- Apt assembler -----------------------------------------------------------

SIMPLE = {name: op for op, name in apt_dis.SIMPLE.items()}
CONST_OPS = {"pushConst": (0xA2, 0xA3), "pushValueOfVar": (0xAE, None),
             "getNamedMember": (0xAF, None), "callNamedFuncPop": (0xB0, None),
             "callNamedMethodPop": (0xB2, None)}
# 0xB1 / 0xB3 call a function / method and store the result with setVariable
# under the name pushed before the arguments; to keep a result on the stack,
# push the name and use callFunction / callMethod.
BRANCHES = {"jump": 0x99, "branchIfTrue": 0x9D, "branchIfFalse": 0xB8}
UNREACHED_FILL = 0x12  # 'not': any simple opcode keeps the stream parseable


class Chunk:
    """Code laid out at a fixed address range [start, end) of an Apt file.

    Instructions are (name, *args); branch targets are label names or
    absolute addresses. If the code ends before `end`, a jump to `then`
    (default `end`) is appended and the rest is filled with unreached bytes.
    """

    def __init__(self, start, end, code, then=None):
        self.start, self.end, self.code = start, end, code
        self.then = end if then is None else then


def pool_index(pool, name):
    key = repr(name)
    if key not in pool:
        raise KeyError(f"{name!r} is not in the constant pool")
    return pool.index(key)


def instr_size(at, ins, pool):
    name = ins[0]
    if name in SIMPLE:
        return 1
    if name in CONST_OPS:
        i = pool_index(pool, ins[1])
        narrow, wide = CONST_OPS[name]
        if i < 256:
            return 2
        if wide is None:
            raise ValueError(f"{name} needs a pool index below 256 ({ins[1]!r} is {i})")
        return 3
    if name in ("pushRegister", "pushByte"):
        return 2
    if name == "pushShort":
        return 3
    if name in BRANCHES or name == "storeRegister":
        return ((at + 1 + 3) & ~3) + 4 - at
    raise ValueError(f"unknown instruction {name}")


def encode(at, ins, pool, labels):
    name = ins[0]
    if name in SIMPLE:
        return bytes([SIMPLE[name]])
    if name in CONST_OPS:
        i = pool_index(pool, ins[1])
        narrow, wide = CONST_OPS[name]
        return bytes([narrow, i]) if i < 256 else bytes([wide]) + struct.pack(">H", i)
    if name == "pushRegister":
        return bytes([0xB9, ins[1]])
    if name == "pushByte":
        return bytes([0xB5]) + struct.pack(">b", ins[1])
    if name == "pushShort":
        return bytes([0xB6]) + struct.pack(">h", ins[1])
    size = instr_size(at, ins, pool)
    pad = bytes(size - 5)
    if name == "storeRegister":
        return bytes([0x87]) + pad + struct.pack(">I", ins[1])
    target = labels[ins[1]] if isinstance(ins[1], str) else ins[1]
    return bytes([BRANCHES[name]]) + pad + struct.pack(">i", target - (at + size))


def assemble(data, chunks, pool):
    """Lays out and writes `chunks` into the bytearray `data`."""
    labels = {}
    layouts = []
    for chunk in chunks:
        at = chunk.start
        placed = []
        for ins in chunk.code:
            if ins[0] == "label":
                labels[ins[1]] = at
                continue
            placed.append((at, ins))
            at += instr_size(at, ins, pool)
        if at != chunk.end and not (placed and placed[-1][1][0] == "jump"):
            ins = ("jump", chunk.then)
            placed.append((at, ins))
            at += instr_size(at, ins, pool)
        if at > chunk.end:
            raise ValueError(f"chunk at {chunk.start:#x} needs {at - chunk.start} bytes, "
                             f"has {chunk.end - chunk.start}")
        layouts.append((chunk, placed))
    for chunk, placed in layouts:
        out = bytearray()
        for at, ins in placed:
            out += encode(at, ins, pool, labels)
        out += bytes([UNREACHED_FILL]) * (chunk.end - chunk.start - len(out))
        data[chunk.start:chunk.end] = out


def frame_pool(apt, action):
    """Constant pool set by the constantPool at the start of an action block."""
    d = apt.d
    if d[action] != 0x88:
        raise ValueError(f"no constantPool at {action:#x}")
    off = (action + 4) & ~3
    n, p = apt.u32(off), apt.u32(off + 4)
    return [apt.const(apt.u32(p + 4 * k)) for k in range(n)]


def expect(data, at, hexbytes):
    want = bytes.fromhex(hexbytes)
    if bytes(data[at:at + len(want)]) != want:
        raise ValueError(f"unexpected bytes at {at:#x}: {bytes(data[at:at + len(want)]).hex()} "
                         f"(want {want.hex()}); wrong game version?")


# --- Patches -----------------------------------------------------------------

def lobby_call(code, arg=None):
    """OSDK_GameSetupAdaptor.GetControllerLogin(code[, arg]): a request to the
    client's team play lobby (src/nhl3on3_team_lobby.cpp), leaving its reply
    on the stack. `code` and `arg` are instruction lists that push a value."""
    return lobby_send(code, arg)[:-1] + [("pushConst", "GetControllerLogin"), ("callMethod",)]


def lobby_send(code, arg=None):
    """lobby_call without the reply."""
    args = [] if arg is None else arg
    return args + code + [("pushByte", 2 if arg else 1), ("pushValueOfVar", "OSDK_GameSetupAdaptor"),
                          ("callNamedMethodPop", "GetControllerLogin")]


# Names a patch needs that its constant pool lacks; the client returns name k
# for GetControllerLogin(200 + k). Must match kScriptNames in
# src/nhl3on3_team_lobby.cpp.
SCRIPT_NAMES = ("lineupPanel", "m_mySide", "mcPositionLock", "getPosition", "setPosition",
                "setConnectedStatus", "setText", "removePadding", "gControllerIDCurrentScreen",
                "mPositions", "NAME", "mainFrame", "SelectSides", "StartingLineups")


def script_name(n):
    """Pushes the string `n` (from SCRIPT_NAMES)."""
    return lobby_call([("pushShort", 200 + SCRIPT_NAMES.index(n))])


def lobby_ready(title):
    """Shows the lobby's title status line: mainFrame.setTitle(status,
    ltxt(title)) when the status changed (the client names the method "-"
    otherwise, which does nothing)."""
    return [*script_name(title), ("pushOne",), ("pushConst", "ltxt"), ("callFunction",),
            *lobby_call([("pushShort", 171)]),
            ("pushByte", 2), *script_name("mainFrame"), ("getVariable",),
            *lobby_call([("pushShort", 170)]), ("callMethod",), ("pop",)]


def disable_calls(data, sites, pool, replacement):
    """Chunks that point the name operand of each call at `sites` (address,
    expected bytes) to `replacement`, a name nothing defines, so the call does
    nothing. The instruction keeps its size."""
    chunks = []
    for at, want in sites:
        expect(data, at, want)
        op = {0xB2: "callNamedMethodPop", 0xA2: "pushConst", 0xA3: "pushConst"}[data[at]]
        chunks.append(Chunk(at, at + len(bytes.fromhex(want)), [(op, replacement)]))
    return chunks


def patch_select_teams(apt_bytes, const_bytes):
    """onlineSelectTeams (the 'SELECT SIDES' screen).

    Stock: each console's side is fixed (host home, the other console away);
    its spare controllers can join it as guests. Left / right on a console's
    own controller did nothing. With more than two consoles one name
    overwrites another in the away panel.

    Patched, with sides kept by the client (src/nhl3on3_team_lobby.cpp):
    - left / right on the console's own controller picks side 0 / 1 (when
      not ready); guest controllers behave as before.
    - custom_UpdateUserAttributes puts player i in panel AWAY_INDEX ^ side(i)
      and that panel shows every player on the side, joined with ', '. Side 0
      (the host's by default) plays as the away (blue, left) team, whose
      panel is AWAY_INDEX; HOME_INDEX is only swapped by online league code.
    - every 250 ms a timer tells the client the player names and the local
      index, and redraws both panels' names, so other consoles' picks show.
    - Ready: stock code marks a panel ready by host / client (the host's
      ready on HOME_INDEX, any client's on AWAY_INDEX). Those calls are
      disabled; the timer reports every player's ready flag and marks a
      side's panel ready once everyone on it is, and in team play the title's
      top line lists who is ready and who is still waited for.

    Space comes from code that is dead in this game: the function's opening
    trace, its interactive league block, the online league side check
    (checkLocalUserSide only acts when m_bOnlineLeague), and the online league
    function updateOnlineLeagueGame, which becomes the timer.
    """
    data = bytearray(apt_bytes)
    apt = apt_dis.Apt(apt_bytes, const_bytes)
    pool = frame_pool(apt, 0xEBC)
    func, body, size = 0x1B5B, 0x1B78, 576
    expect(data, func, "8e")
    expect(data, body, "a2d7aea3b9034e")           # trace(... 'custom_UpdateUserAttributes [' ...)
    expect(data, 0x1BED, "aea3b9034eafdd")         # m_aPlayers[i].core_bIsGuest
    expect(data, 0x1BFC, "aea3b9034eafd2")         # m_aPlayers[i].core_bIsHost
    expect(data, 0x1C12, "87000000000517")         # storeRegister 5; pop
    expect(data, 0x1C19, "735aae11b9054eb2de")     # m_arrPanels[r5].showGamerTag(true)
    expect(data, 0x1C22, "aea3b9034eafb85aae11b9054eb2df")  # ...setGamerTag(core_username)
    expect(data, 0x1C31, "aea3b9034eafe0")         # m_aPlayers[i].core_iNumGuests
    expect(data, 0x1C86, "b903ae21499d")           # i == HOST_INDEX ? HOME_SIDE : AWAY_SIDE
    # defaultLocalEventHandler, UI_LEFT and UI_RIGHT cases: if controller is
    # not the lead and ... then updateController(LEFT / RIGHT, ...); return true
    left, right, cases_end = 0x33C0, 0x3410, 0x3460
    expect(data, left, "b903b901afa949124c12")
    expect(data, right, "b903b901afa949124c12")
    expect(data, cases_end, "b9035aae13")         # OPTION_TOP case: m_mcControllers.getControllerSide
    timer_func, timer_body, timer_end = 0x45B9, 0x45D8, 0x477E
    expect(data, timer_func, "8e")
    expect(data, timer_body, "a301f95ab002")      # _gsTrace('**** ...updateOnlineLeagueGame')
    expect(data, timer_end, "8e")

    # Name of the variable holding the timer's interval id: a pool string no
    # code uses as a name. It must be a plain identifier: getVariable and
    # setVariable read a name with ':' in it as a target path.
    timer_var = "TXT_ARCADE_CHANGE_CONTROLLER_SETTINGS"
    me = [("pushConst", "m_iMyUserIndex"), ("getVariable",)]
    stop_timer = [("pushConst", timer_var), ("getVariable",), ("pushOne",),
                  ("pushConst", "clearInterval"), ("callFunctionPop",)]
    pre_exit = 0x1234  # custom_PreOnExitScreen: _gsTrace('...custom_PreExitScreen ' + playerInfo0)
    expect(data, pre_exit, "a2")
    expect(data, pre_exit + 10, "ae")

    i, panel = 3, 5  # custom_UpdateUserAttributes registers
    chunks = [
        Chunk(body, body + 8, [("jump", "entry")]),
        Chunk(body + 8, 0x1BED, [
            ("label", "entry"),
            # register player i's name
            *lobby_send([("pushRegister", i), ("pushShort", 1000), ("add2",)],
                        [("pushValueOfVar", "m_aPlayers"), ("pushRegister", i), ("getMember",),
                         ("getNamedMember", "core_username")]),
            # start the timer once
            ("pushConst", timer_var), ("getVariable",), ("pushUndefined",), ("equals2",),
            ("branchIfFalse", 0x1BED),
            # setInterval(updateOnlineLeagueGame, 250, m_thisScreen): this
            # engine's setInterval reads a third argument even when not given
            ("pushConst", timer_var), ("pushValueOfVar", "m_thisScreen"),
            ("pushShort", 250), ("pushConst", "updateOnlineLeagueGame"), ("getVariable",),
            ("pushByte", 3), ("pushConst", "setInterval"), ("callFunction",),
            ("setVariable",),
            ("jump", 0x1BED),
            # panel shows the names on its side
            ("label", "names"),
            *lobby_call([("pushRegister", panel), ("pushValueOfVar", "AWAY_INDEX"), ("bitXor",),
                         ("pushByte", 110), ("add2",)]),
            ("pushOne",), ("pushValueOfVar", "m_arrPanels"), ("pushRegister", panel),
            ("getMember",), ("callNamedMethodPop", "setGamerTag"),
            ("jump", 0x1C31),
            # panel = AWAY_INDEX ^ side(i)
            ("label", "side"),
            *lobby_call([("pushRegister", i), ("pushByte", 120), ("add2",)]),
            ("jump", "side_end"),
        ]),
        Chunk(0x1BFC, 0x1C19, [
            ("jump", "side"),
            ("label", "side_end"),
            ("toNumber",), ("pushValueOfVar", "AWAY_INDEX"), ("bitXor",),
            ("storeRegister", panel), ("pop",),
        ]),
        Chunk(0x1C22, 0x1C31, [("jump", "names")]),
        # the local controller's puck: SIDES[panel], not by host / client
        Chunk(0x1C86, 0x1C9A, [
            ("pushConst", "SIDES"), ("getVariable",), ("pushRegister", panel), ("getMember",),
        ]),

        # Input. Stack on entry to "pick": direction, lobby code. Registers:
        # 0 (switch temp, free here), 1 _global, 3 controllerId, 5 own side.
        Chunk(left, right, [
            ("pushValueOfVar", "LEFT"), ("pushByte", 100),
            ("label", "pick"),
            ("storeRegister", 6), ("pop",), ("storeRegister", 0), ("pop",),
            ("pushZero",), ("pushValueOfVar", "m_thisScreen"), ("pushConst", "isLocalUserReady"),
            ("callMethod",), ("branchIfTrue", "handled"),
            ("pushRegister", 3), ("pushRegister", 1),
            ("getNamedMember", "gControllerIDCurrentScreen"), ("equals2",),
            ("branchIfFalse", "guest"),
            *lobby_send([("pushRegister", 6)], me),
            ("pushTrue",), ("return",),
        ]),
        Chunk(right, cases_end, [
            ("pushValueOfVar", "RIGHT"), ("pushByte", 101), ("jump", "pick"),
            ("label", "guest"),
            ("pushConst", "m_ControllersPerSide"), ("getVariable",), ("pushOne",), ("greater",),
            ("branchIfFalse", "handled"),
            ("pushValueOfVar", "m_bJerseyMode"), ("branchIfTrue", "handled"),
            ("pushRegister", 0), ("pushRegister", 3), ("pushRegister", 5), ("pushByte", 3),
            ("pushConst", "updateController"), ("callFunctionPop",),
            ("label", "handled"),
            ("pushTrue",), ("return",),
        ]),

        # Stop the timer when leaving the screen (fills the trace exactly)
        Chunk(pre_exit, pre_exit + 10, stop_timer + [("pushZero",), ("pop",)]),

        # The timer (registers 2..4 are free; 1 is _global).
        Chunk(timer_body, timer_end, [
            ("pushValueOfVar", "m_bLeavingScreen"), ("branchIfFalse", "tick"),
            *stop_timer,
            ("pushUndefined",), ("return",),
            ("label", "tick"),
            *lobby_send([("pushByte", 99)],
                        me + [("pushByte", 16), ("multiply",), ("pushValueOfVar", "m_aPlayers"),
                              ("getNamedMember", "length"), ("add2",)]),
            ("pushZero",), ("storeRegister", 2), ("pop",),
            ("label", "player"),
            ("pushRegister", 2), ("pushValueOfVar", "m_aPlayers"), ("getNamedMember", "length"),
            ("less2",), ("branchIfFalse", "panels"),
            *lobby_send([("pushRegister", 2), ("pushShort", 1000), ("add2",)],
                        [("pushValueOfVar", "m_aPlayers"), ("pushRegister", 2), ("getMember",),
                         ("getNamedMember", "core_username")]),
            *lobby_send([("pushRegister", 2), ("pushShort", 1200), ("add2",)],
                        [("pushRegister", 2), ("pushOne",), ("pushConst", "isUserReady"),
                         ("callFunction",)]),
            ("pushRegister", 2), ("increment",), ("storeRegister", 2), ("pop",),
            ("jump", "player"),
            ("label", "panels"),
            *[ins for side in (0, 1) for ins in [
                *lobby_call([("pushByte", 110 + side)]),
                ("pushOne",), ("pushValueOfVar", "m_arrPanels"),
                ("pushValueOfVar", "AWAY_INDEX"), ("pushByte", side), ("bitXor",), ("getMember",),
                ("callNamedMethodPop", "setGamerTag"),
                # panel.setReady(everyone on the side ready), when that changed
                *lobby_call([("pushShort", 164 + side)]), ("toNumber",),
                ("pushOne",), ("pushValueOfVar", "m_arrPanels"),
                ("pushValueOfVar", "AWAY_INDEX"), ("pushByte", side), ("bitXor",), ("getMember",),
                *lobby_call([("pushShort", 160 + side)]), ("callMethod",), ("pop",),
            ]],
            *lobby_ready("SelectSides"),
            # move the local puck when the local side changed
            *lobby_call([("pushByte", 102)]), ("toNumber",), ("storeRegister", 3), ("pop",),
            ("pushRegister", 3), ("pushZero",), ("less2",), ("branchIfTrue", "end"),
            ("pushTrue",), ("pushTrue",),
            ("pushConst", "SIDES"), ("getVariable",),
            ("pushValueOfVar", "AWAY_INDEX"), ("pushRegister", 3), ("bitXor",), ("getMember",),
            ("pushRegister", 1), ("getNamedMember", "EA_ZONE"),
            ("getNamedMember", "gControllerIDCurrentScreen"),
            ("pushByte", 4), ("pushValueOfVar", "m_mcControllers"),
            ("callNamedMethodPop", "setControllerSide"),
            ("label", "end"),
            ("pushUndefined",), ("return",),
        ]),
    ]
    # Stock panel ready marks: custom_HandleReadyMessage (2),
    # custom_UpdateUserAttributes, clearReadyFlags (2), parseSetData (2), and
    # the host's own unready / ready.
    set_ready = "b2bd"  # callNamedMethodPop 'setReady'
    ready_sites = [(at, set_ready) for at in (0x1935, 0x194A, 0x1C47, 0x291F, 0x2928, 0x2D95,
                                              0x2DBA, 0x350B, 0x3C0E)]
    chunks += disable_calls(data, ready_sites, pool, " HOST_INDEX ")
    assemble(data, chunks, pool)
    return bytes(data), [(func, body + size), (left, cases_end), (timer_func, timer_end),
                         (0x1214, 0x125E)] + [(at, at + 2) for at, _ in ready_sites], 0xEBC


def patch_starting_lineups(apt_bytes, const_bytes):
    """onlineStartingLineups (STARTING LINEUPS, with the position lock).

    Stock: the position lock (mcPositionLock, the nhlcontrollers component)
    shows an icon per local controller with the skater it is locked to; a
    console never learns other consoles' locks until the match starts.

    Patched, with locks kept by the client (src/nhl3on3_team_lobby.cpp):
    - every 250 ms a timer reports the local controller's lock, so it reaches
      the other consoles while the screen is up.
    - the lock's spare icons (4..6; only getMaxControllers() = 4 are local)
      show the local side's players on other consoles, each with the skater
      it locked and its name, "SKATER (name)". An icon holds its skater like
      a local controller's, so the local player cannot pick a taken one.
    - updateUserList tells the client which players are on this console.
    - Ready: stock code shows a side's READY when any one player there
      readies, and on the host a client's ready always goes to the other
      side. Those calls are disabled; the timer shows a side's READY once
      everyone on it is ready (from the ready messages the client reads off
      the wire), and in team play the title's top line lists who is ready
      and who is still waited for.

    The screen's second action block holds updateUserList and two functions
    that only run for online leagues: LeagueEventHandler (becomes the timer)
    and handleLeagueRosterCRC (updates one teammate icon). That block's
    constant pool lacks the lock's names, so the client returns them as
    strings (GetControllerLogin(200 + k)) for getVariable / getMember /
    callMethod to use.
    """
    data = bytearray(apt_bytes)
    apt = apt_dis.Apt(apt_bytes, const_bytes)
    pool_at = 0x4DB4
    pool = frame_pool(apt, pool_at)
    users, users_end = 0x6344, 0x63A0     # updateUserList: the per-user traces
    expect(data, users, "a30134ae56b9044e")  # trace('UserName: ' + m_aPlayers[r4]...
    expect(data, users_end, "b90450")        # r4++
    icon_func, icon_body, icon_end = 0x746D, 0x748C, 0x7584
    expect(data, icon_func, "8e")
    expect(data, icon_body, "a301c3b9074726")  # trace(':::handleLeagueRosterCRC...' + r7)
    expect(data, icon_end, "8e")
    timer_func, timer_body, timer_end = 0x953C, 0x955C, 0x9680
    expect(data, timer_func, "8e")
    expect(data, timer_body, "a302b7b90447")   # trace('[GameSetup]::LeagueEventHandler...'
    expect(data, timer_end, "a301ae")

    # Variables for the timer id and the lock: event names only the replaced
    # league handler compared against.
    timer_var, lock_var = "EVENT_OL_LOAD_ROSTER_SUCCESS", "EVENT_OL_LOAD_MEMBERS_SUCCESS"
    name = script_name

    def call(obj, method, args):
        """obj.method(*args), dropping the result."""
        out = []
        for arg in reversed(args):
            out += arg
        return out + [("pushByte", len(args)), *obj, *name(method), ("callMethod",), ("pop",)]

    lock = [("pushRegister", 4)]
    slot = [("pushRegister", 5)]
    chunks = [
        Chunk(users, users_end, [
            *lobby_call([("pushRegister", 4), ("pushShort", 1100), ("add2",)],
                        [("pushValueOfVar", "m_aPlayers"), ("pushRegister", 4), ("getMember",),
                         ("getNamedMember", "core_bIsLocalUser")]),
            ("pop",),
            # start the timer once
            ("pushConst", timer_var), ("getVariable",), ("pushUndefined",), ("equals2",),
            ("branchIfFalse", users_end),
            ("pushConst", timer_var), ("pushValueOfVar", "m_thisScreen"),
            ("pushShort", 250), ("pushConst", "LeagueEventHandler"), ("getVariable",),
            ("pushByte", 3), ("pushConst", "setInterval"), ("callFunction",),
            ("setVariable",),
        ]),

        # The timer. Registers: 1 _root; 2 the lock.
        Chunk(timer_body, timer_end, [
            ("pushValueOfVar", "m_bLeavingScreen"), ("branchIfFalse", "tick"),
            ("pushConst", timer_var), ("getVariable",), ("pushOne",),
            ("pushConst", "clearInterval"), ("callFunctionPop",),
            ("pushUndefined",), ("return",),
            ("label", "tick"),
            # lock = lineupPanel[m_mySide].mcPositionLock
            *name("lineupPanel"), ("getVariable",), *name("m_mySide"), ("getVariable",),
            ("getMember",), *name("mcPositionLock"), ("getMember",),
            ("storeRegister", 2), ("pop",),
            ("pushConst", lock_var), ("pushRegister", 2), ("setVariable",),
            # report lock.getPosition(_root.gControllerIDCurrentScreen)
            *lobby_call([("pushShort", 130)],
                        [("pushRegister", 1), *name("gControllerIDCurrentScreen"), ("getMember",),
                         ("pushOne",), ("pushRegister", 2), *name("getPosition"), ("callMethod",)]),
            ("pop",),
            *[ins for j in range(3) for ins in [
                ("pushByte", j), ("pushOne",), ("pushConst", "handleLeagueRosterCRC"),
                ("callFunctionPop",)]],
            # animateReady / animateNotReady(screen side) when a team side's
            # "everyone ready" changed; screen side = 1 - team side
            *[ins for side in (0, 1) for ins in [
                ("pushByte", 1 - side), ("pushOne",), *lobby_call([("pushShort", 162 + side)]),
                ("callFunction",), ("pop",)]],
            *lobby_ready("StartingLineups"),
            ("pushUndefined",), ("return",),
        ]),

        # Teammate icon j (r7). Registers: 3 position (-1 hidden, -2 unchanged),
        # 4 the lock, 5 icon (4 + j), 6 shown.
        Chunk(icon_body, icon_end, [
            *lobby_call([("pushRegister", 7), ("pushShort", 140), ("add2",)]),
            ("toNumber",), ("storeRegister", 3), ("pop",),
            ("pushRegister", 3), ("pushByte", -1), ("less2",), ("branchIfTrue", "icon_end"),
            ("pushConst", lock_var), ("getVariable",), ("storeRegister", 4), ("pop",),
            ("pushRegister", 7), ("pushByte", 4), ("add2",), ("storeRegister", 5), ("pop",),
            ("pushRegister", 3), ("pushZero",), ("less2",), ("not",), ("storeRegister", 6),
            ("pop",),
            ("pushRegister", 6), ("branchIfTrue", "shown"),
            ("pushZero",), ("storeRegister", 3), ("pop",),
            ("label", "shown"),
            *call(lock, "setPosition", [slot, [("pushRegister", 3)]]),
            # setText(icon, mPositions[position].NAME + " (name)")
            *call(lock, "setText", [slot, [
                ("pushRegister", 4), *name("mPositions"), ("getMember",), ("pushRegister", 3),
                ("getMember",), *name("NAME"), ("getMember",),
                *lobby_call([("pushRegister", 7), ("pushShort", 150), ("add2",)]), ("add2",)]]),
            *call(lock, "setConnectedStatus", [slot, [("pushRegister", 6)]]),
            *call(lock, "removePadding", []),
            ("label", "icon_end"),
            ("pushUndefined",), ("return",),
        ]),
    ]
    assemble(data, chunks, pool)

    # First action block (its own constant pool): sides by team, not by host.
    # Stock: m_mySide = host ? HOME : AWAY, and lineup messages from another
    # console go to m_otherSide. Patched: m_mySide is the lead controller's
    # side, which Select Sides set from the team pick, and a lineup message
    # goes to its player's side. Team side 0 plays away (SIDES[AWAY_INDEX] on
    # Select Sides), and AWAY is 1, so the screen side is 1 - team side.
    pool1 = frame_pool(apt, 0x1300)
    my_side, my_side_end = 0x2754, 0x2783   # custom_PostInitScreen
    expect(data, my_side, "a28faea79d")      # m_mySide = m_bIsHost ? HOME : AWAY
    expect(data, 0x276B, "a3010aae8f")       # m_otherSide = m_mySide == HOME ? AWAY : HOME
    expect(data, my_side_end, "5987")        # r3 = 0
    set_side, set_side_end = 0x370D, 0x372A  # custom_HandleSetMessage: r11 = side
    expect(data, set_side, "b90a594e59a30106")
    expect(data, set_side_end, "87")
    upd_side, upd_side_end = 0x38B1, 0x38CE  # custom_handleUpdateMessage: r6 = side
    expect(data, upd_side, "b905594e59a30106")
    expect(data, upd_side_end, "87")

    def side_of_player(fields):
        """1 - team side of player fields[0] (the user index a message is about)."""
        return [("pushOne",), ("pushRegister", fields), ("pushZero",), ("getMember",),
                ("toNumber",), ("pushByte", 120), ("add2",), ("pushOne",),
                ("pushConst", "OSDK_GameSetupAdaptor"), ("getVariable",),
                ("pushConst", "GetControllerLogin"), ("callMethod",), ("subtract",)]

    # Stock READY marks, per side for any one player: the host's own unready /
    # ready (m_mySide), a client's ready on the host (m_otherSide, wrong for a
    # teammate) and the host's update on clients.
    animate_ready, animate_not_ready = "a301b9", "a301af"
    ready_sites = [(0x3521, animate_not_ready), (0x3605, animate_ready),
                   (0x361E, animate_not_ready), (0x3995, animate_ready),
                   (0x39AE, animate_not_ready), (0x3CE9, animate_ready)]
    assemble(data, [
        *disable_calls(data, ready_sites, pool1, "<no last screen id>"),
        Chunk(my_side, my_side_end, [
            ("pushConst", "m_mySide"),
            ("pushRegister", 1), ("pushConst", "gControllerIDCurrentScreen"), ("getMember",),
            ("pushOne",), ("pushRegister", 1), ("getNamedMember", "ssm"),
            ("pushConst", "getControllerSide"), ("callMethod",), ("setVariable",),
            ("pushConst", "m_otherSide"), ("pushOne",), ("pushValueOfVar", "m_mySide"),
            ("subtract",), ("setVariable",),
        ]),
        Chunk(set_side, set_side_end, side_of_player(10)),
        Chunk(upd_side, upd_side_end, side_of_player(5)),
    ], pool1)
    return bytes(data), [(users - 0x30, users_end + 0x10), (icon_func, icon_end),
                         (timer_func, timer_end)], pool_at


def patch_pimp_full_nhl(apt_bytes, const_bytes):
    """pimpFullNHL (GET THE NHL 09 EXPERIENCE), the advert the main menu's
    quit item (RETURN TO GAME LIBRARY) opens; A asks "return to game
    library?" and YES calls ION_Arcade.QuitArcadeGame.

    Patched: initGUI_2, which runs on frame 2 while initGUI_1 still has the
    screen hidden, calls ION_Arcade.QuitArcadeGame() itself, so the game
    exits without showing the advert or the prompt.
    """
    data = bytearray(apt_bytes)
    apt = apt_dis.Apt(apt_bytes, const_bytes)
    pool_at, body, body_end = 0x5B8, 0x60C, 0x624
    expect(data, body, "745ab901af02b203b502a204b502b901a205525aae06b207")
    pool = frame_pool(apt, pool_at)
    # Register 1 is _global (the function preloads only it).
    assemble(data, [Chunk(body, body_end, [
        ("pushZero",), ("pushRegister", 1), ("getNamedMember", "ION_Arcade"),
        ("callNamedMethodPop", "QuitArcadeGame"),
    ])], pool)
    return bytes(data), [(0x5ED, body_end)], pool_at


# (archive entry, nested entry of the .apt, nested entry of the .const, patch)
PATCHES = [
    ("fe/ion/game/screens/misc/pimpfullnhl.big", "0", "1", patch_pimp_full_nhl),
    ("fe/ion/game/screens/online/onlineselectteams.big", "0", "1", patch_select_teams),
    ("fe/ion/game/screens/online/onlinestartinglineups.big", "0", "1", patch_starting_lineups),
]

# --- Archive rebuild ---------------------------------------------------------

# Written by the game beside data0.big once it has applied its patches.
MARKER = "data0.nhl3on3"
DEFAULT_EMIT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src",
                            "nhl3on3_ui_patches.inc")


def big_directory(data):
    """[(name, offset, size, directory position of the offset field)]"""
    count = struct.unpack(">I", data[8:12])[0]
    p = 16
    out = []
    for _ in range(count):
        off, size = struct.unpack(">II", data[p:p + 8])
        end = data.index(b"\0", p + 8)
        out.append((data[p + 8:end].decode("latin1"), off, size, p))
        p = end + 1
    return out


def patched_screens(big, check):
    """Yields (archive entry name, new uncompressed bytes, original
    uncompressed bytes)."""
    directory = {name: (off, size) for name, off, size, _ in big_directory(big)}
    for entry, apt_name, const_name, patch in PATCHES:
        off, size = directory[entry]
        original = ea_big.unpack(big[off:off + size])
        nested = bytearray(original)
        inner = {name: (o, s) for name, o, s, _ in big_directory(nested)}
        (ao, asz), (co, csz) = inner[apt_name], inner[const_name]
        apt_bytes, const_bytes = bytes(nested[ao:ao + asz]), bytes(nested[co:co + csz])
        new_apt, ranges, pool_at = patch(apt_bytes, const_bytes)
        if len(new_apt) != asz:
            raise ValueError(f"{entry}: patch changed the Apt size")
        nested[ao:ao + asz] = new_apt
        print(f"patched {entry}")
        if check:
            apt = apt_dis.Apt(new_apt, const_bytes)
            for start, end in ranges:
                out = io.StringIO()
                apt.disasm(start, end, out=out, pool=frame_pool(apt, pool_at))
                print(out.getvalue())
        yield entry, bytes(nested), original


def rebuild(game_dir, check):
    big_path, bh_path = os.path.join(game_dir, "data0.big"), os.path.join(game_dir, "data0.bh")
    for path in (big_path, bh_path):
        if not os.path.exists(path + ".orig"):
            shutil.copy2(path, path + ".orig")
            print(f"saved {path}.orig")
    big = bytearray(open(big_path + ".orig", "rb").read())
    bh = bytearray(open(bh_path + ".orig", "rb").read())
    directory = big_directory(big)
    index = {off: k for k, (_, off, _, _) in enumerate(directory)}
    positions = {name: (off, pos) for name, off, _, pos in directory}
    for entry, blob, _ in patched_screens(big, check):
        old_off, pos = positions[entry]
        new_off = (len(big) + 63) & ~63
        big += bytes(new_off - len(big)) + blob
        struct.pack_into(">II", big, pos, new_off, len(blob))
        k = index[old_off]
        bh_entry = 16 + 16 * k
        if struct.unpack(">I", bh[bh_entry:bh_entry + 4])[0] != old_off:
            raise ValueError(f"{entry}: .bh entry {k} does not match the .big directory")
        struct.pack_into(">III", bh, bh_entry, new_off, len(blob), 0)
    struct.pack_into("<I", big, 4, len(big))
    if check:
        return
    open(big_path, "wb").write(big)
    open(bh_path, "wb").write(bh)
    marker = os.path.join(game_dir, MARKER)
    if os.path.exists(marker):
        os.remove(marker)  # the game re-applies its own copy of the patches
    print(f"wrote {big_path} and {bh_path}")


def emit_cpp(game_dir, out_path):
    """Writes the patches as byte runs for the game to apply at startup
    (src/nhl3on3_ui_patch.cpp): only the bytes the patches write, with the
    size and CRC-32 of each original uncompressed screen to check against."""
    big_path = os.path.join(game_dir, "data0.big")
    if os.path.exists(big_path + ".orig"):
        big_path += ".orig"
    big = open(big_path, "rb").read()
    lines = ["// Generated by `tools/ui_mod.py GAME_DIR --emit-cpp`; do not edit.",
             "// Included by src/nhl3on3_ui_patch.cpp.", ""]
    screens = []
    version = 0
    for k, (entry, new, original) in enumerate(patched_screens(big, False)):
        if len(new) != len(original):
            raise ValueError(f"{entry}: patch changed the screen size")
        runs, i = [], 0
        while i < len(new):
            if new[i] == original[i]:
                i += 1
                continue
            start = i
            while i < len(new) and new[i:i + 8] != original[i:i + 8]:
                i += 1
            runs.append((start, new[start:i]))
        for r, (start, data) in enumerate(runs):
            body = ", ".join(f"0x{b:02X}" for b in data)
            lines.append(f"constexpr uint8_t kScreen{k}Run{r}[] = {{{body}}};")
            version = zlib.crc32(struct.pack("<I", start) + data, version)
        lines.append(f"constexpr Run kScreen{k}Runs[] = {{")
        lines += [f"    {{{start:#x}, sizeof(kScreen{k}Run{r}), kScreen{k}Run{r}}},"
                  for r, (start, _) in enumerate(runs)]
        lines += ["};", ""]
        crc = zlib.crc32(original)
        version = zlib.crc32(entry.encode() + struct.pack("<I", crc), version)
        screens.append(f'    {{"{entry}", {len(original)}, 0x{crc:08X}, kScreen{k}Runs, '
                       f'std::size(kScreen{k}Runs)}},')
    lines += ["constexpr Screen kScreens[] = {", *screens, "};", "",
              f"constexpr uint32_t kPatchVersion = 0x{version:08X};", ""]
    with open(out_path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"wrote {out_path} ({len(screens)} screens, version {version:08X})")


def restore(game_dir):
    for name in ("data0.big", "data0.bh"):
        path = os.path.join(game_dir, name)
        if os.path.exists(path + ".orig"):
            shutil.copy2(path + ".orig", path)
            print(f"restored {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("--restore", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--emit-cpp", nargs="?", const=DEFAULT_EMIT, metavar="PATH")
    args = ap.parse_args()
    if args.emit_cpp:
        emit_cpp(args.game_dir, args.emit_cpp)
    elif args.restore:
        restore(args.game_dir)
    else:
        rebuild(args.game_dir, args.check)


if __name__ == "__main__":
    main()
