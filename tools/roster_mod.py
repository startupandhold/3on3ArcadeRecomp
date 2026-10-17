#!/usr/bin/env python3
"""Adds players to the 3-on-3 rosters from a JSON file of names, teams,
types and headshots (see game/roster/roster.json).

Usage: roster_mod.py [GAME_DIR] [--roster PATH] [--dry-run] [--restore]

GAME_DIR is the extracted game (default game/extracted). Run the game once
first so its front-end mod is in data0.big; this tool builds on top of it.

What it changes:
  db/nhlng.db       player records (cPbu) and, for new players, ratings
                    copied from a star of the same type and position.
  data0.big / .bh   a large and small portrait per player, appended to the
                    archive under fe/ion/artassets/playerheads[small]/.

Players already in the database (matched by name) are updated in place;
anyone else takes over a free agent's record. A player joining a team sends
that team's lowest-rated player in the same position group (forward,
defence, goalie) to free agency, so roster sizes stay the same. Players
without a headshot file are skipped unless they already have a portrait.

Every run rebuilds from backups: db/nhlng.db.orig, and data0.big.preroster /
data0.bh.preroster (the archive as the game left it). When the game updates
its front-end mod it rebuilds data0.big without the portraits; run this tool
again afterwards. --restore puts the backups back.
"""
import argparse
import json
import os
import shutil
import struct
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ea_big  # noqa: E402
import xenos_texture  # noqa: E402
from ea_tdb import Database  # noqa: E402

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_GAME = os.path.join(REPO, "game", "extracted")
DEFAULT_ROSTER = os.path.join(REPO, "game", "roster", "roster.json")

TEAMS = ["ANA", "ATL", "BOS", "BUF", "CGY", "CAR", "CHI", "COL", "CBJ", "DAL", "DET", "EDM", "FLA",
         "LA", "MIN", "MTL", "NSH", "NJ", "NYI", "NYR", "OTT", "PHI", "PHX", "PIT", "STL", "SJ", "TB",
         "TOR", "VAN", "WSH"]
POSITIONS = ["C", "LW", "RW", "D", "G"]
TYPES = ("all_around", "fast", "strong")

# cPbu (players) fields.
FIRST, LAST, BIRTHPLACE = "PedH", "RMbQ", "JzFM"
ID, ASSET_ID, KEY = "Nzao", "rnOl", "zIBw"
TEAM, NHL_TEAM = "BSXd", "WBbd"  # NHL_TEAM is team + 1, or 0 for a free agent
POSITION, NUMBER = "aljv", "tRVs"
BIRTH_YEAR, BIRTH_MONTH, BIRTH_DAY = "dnFq", "pLKJ", "iwsK"  # year - 1900
SKATER_RATINGS, GOALIE_RATINGS = ("yvSd", "ajmx"), ("yuHm",)
PORTRAIT_IDS = range(0, 4000)  # the p0_4000 folder

# Stars whose ratings new players start from, by (type, position group).
TEMPLATES = {
    ("all_around", "F"): "Sidney Crosby", ("fast", "F"): "Patrick Kane",
    ("strong", "F"): "Jarome Iginla", ("all_around", "D"): "Nicklas Lidstrom",
    ("fast", "D"): "Mike Green", ("strong", "D"): "Zdeno Chara",
    ("all_around", "G"): "Henrik Lundqvist", ("fast", "G"): "Henrik Lundqvist",
    ("strong", "G"): "Henrik Lundqvist",
}

PORTRAITS = [
    # (archive folder, texture size, displayed size)
    ("fe/ion/artassets/playerheads/p0_4000", (512, 256), (467, 253)),
    ("fe/ion/artassets/playerheadssmall/p0_4000", (256, 128), (202, 128)),
]
PORTRAIT_TEMPLATE_ID = 1082
MARKER = "data0.nhl3on3"
STATE = "data0.roster"


def normalize(name):
    decomposed = unicodedata.normalize("NFKD", name)
    return "".join(c for c in decomposed if not unicodedata.combining(c)).casefold().strip()


def group(position):
    return "G" if position == 4 else "D" if position == 3 else "F"


# --- Archive ------------------------------------------------------------------

def djb2(name):
    h = 5381
    for c in name.lower().encode("latin1"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


class Archive:
    """data0.big with its data0.bh index: entries can be added or replaced,
    and save() rewrites the directory (shifting the data if it grows)."""

    def __init__(self, big, bh):
        self.big, self.bh = big, bh
        count = struct.unpack_from(">I", big, 8)[0]
        self.entries = []  # [name, offset, size, unpacked size, new data or None]
        p = 16
        for k in range(count):
            off, size = struct.unpack_from(">II", big, p)
            end = big.index(b"\0", p + 8)
            bh_off, _, unpacked, _ = struct.unpack_from(">IIII", bh, 16 + 16 * k)
            if bh_off != off:
                raise ValueError(f"data0.bh entry {k} does not match data0.big")
            self.entries.append([big[p + 8:end].decode("latin1"), off, size, unpacked, None])
            p = end + 1
        self.directory_end = p
        self.trailer = big[p:p + 8]
        self.first_data = min(e[1] for e in self.entries)
        self.data_end = max(e[1] + e[2] for e in self.entries)
        self.index = {e[0]: e for e in self.entries}

    def read(self, name):
        _, off, size, _, data = self.index[name]
        return data if data is not None else ea_big.unpack(bytes(self.big[off:off + size]))

    def put(self, name, data):
        if name in self.index:
            self.index[name][4] = data
        else:
            entry = [name, 0, 0, 0, data]
            self.entries.append(entry)
            self.index[name] = entry

    def save(self):
        directory = b"".join(struct.pack(">II", 0, 0) + e[0].encode("latin1") + b"\0" for e in self.entries)
        directory_end = 16 + len(directory)
        shift = max(0, directory_end + len(self.trailer) - self.first_data)
        shift = (shift + 4095) & ~4095
        big = bytearray(self.first_data + shift)
        big += self.big[self.first_data:self.data_end]
        for e in self.entries:
            if e[4] is None:
                e[1] += shift
            else:
                big += bytes(-len(big) % 64)
                e[1], e[2], e[3] = len(big), len(e[4]), 0
                big += e[4]
        p = 16
        for e in self.entries:
            name = e[0].encode("latin1") + b"\0"
            big[p:p + 8 + len(name)] = struct.pack(">II", e[1], e[2]) + name
            p += 8 + len(name)
        big[p:p + len(self.trailer)] = self.trailer
        big[0:16] = b"BIG4" + struct.pack("<I", len(big)) + struct.pack(">II", len(self.entries), p + 8)
        bh = bytearray(16 + 16 * len(self.entries) + 16)
        bh[0:16] = b"Viv4" + struct.pack("<I", len(bh)) + struct.pack(">II", len(self.entries), len(bh))
        for k, e in enumerate(self.entries):
            struct.pack_into(">IIII", bh, 16 + 16 * k, e[1], e[2], e[3], djb2(e[0]))
        struct.pack_into(">I", bh, len(bh) - 16, len(big))
        return big, bh


def portrait_package(archive, folder, texture_size, display_size, image):
    """A portrait .big built from the template player's, with new pixels."""
    tw, th = texture_size
    nested = bytearray(archive.read(f"{folder}/p{PORTRAIT_TEMPLATE_ID}.big"))
    for name, off, size in ea_big.entries(nested):
        if nested[off:off + 4] == b"\x89RW4" and size == 4096 + tw * th:
            break
    else:
        raise ValueError(f"{folder}/p{PORTRAIT_TEMPLATE_ID}.big has no {tw}x{th} texture")
    width, height, rgba = image
    dw, dh = display_size
    scaled_w = max(dw, round(width * dh / height))
    scaled = xenos_texture.resize(rgba, width, height, scaled_w, dh)
    cropped = xenos_texture.place(scaled, scaled_w, dh, dw, dh, x=-((scaled_w - dw) // 2))
    canvas = xenos_texture.place(cropped, dw, dh, tw, th)
    nested[off + 4096:off + size] = xenos_texture.encode_dxt5(canvas, tw, th)
    return bytes(nested)


# --- Database -----------------------------------------------------------------

class Players:
    def __init__(self, db):
        self.db = db
        self.table = db["cPbu"]
        self.rows = self.table.rows()
        self.ratings = {}
        for name in SKATER_RATINGS + GOALIE_RATINGS:
            t = db[name]
            self.ratings[name] = {t.get(r, KEY): r for r in range(t.count)}

    def find(self, first, last):
        want = (normalize(first), normalize(last))
        hits = [r for r, p in enumerate(self.rows) if (normalize(p[FIRST]), normalize(p[LAST])) == want]
        hits.sort(key=lambda r: (self.rows[r][NHL_TEAM] == 0, not self.has_ratings(r)))
        return hits[0] if hits else None

    def rating_tables(self, row):
        return GOALIE_RATINGS if self.rows[row][POSITION] == 4 else SKATER_RATINGS

    def has_ratings(self, row):
        key = self.rows[row][KEY]
        return all(key in self.ratings[t] for t in self.rating_tables(row))

    def overall(self, row):
        t = self.db[self.rating_tables(row)[0]]
        r = self.ratings[t.name][self.rows[row][KEY]]
        values = [t.get(r, f.name) for f in t.fields.values() if f.bits == 6]
        return sum(values) / max(1, len(values))

    def set(self, row, field, value):
        self.table.set(row, field, value)
        self.rows[row][field] = value

    def copy_ratings(self, source, target):
        for name in self.rating_tables(source):
            t = self.db[name]
            src = self.ratings[name][self.rows[source][KEY]]
            dst = self.ratings[name][self.rows[target][KEY]]
            for field in t.fields:
                if field != KEY:
                    t.set(dst, field, t.get(src, field))


def validate(player, k):
    where = f"player {k + 1} ({player.get('first_name', '')} {player.get('last_name', '')})"
    for field in ("first_name", "last_name", "team", "position", "jersey_number"):
        if player.get(field) in (None, ""):
            raise ValueError(f"{where}: {field} is missing")
    if player["team"] not in TEAMS:
        raise ValueError(f"{where}: unknown team {player['team']!r}")
    if player["position"] not in POSITIONS:
        raise ValueError(f"{where}: unknown position {player['position']!r}")
    if player["position"] != "G" and player.get("type") not in TYPES:
        raise ValueError(f"{where}: type must be one of {', '.join(TYPES)}")
    if not 0 <= int(player["jersey_number"]) <= 99:
        raise ValueError(f"{where}: jersey_number must be 0-99")


def apply_players(players, roster, has_portrait, log):
    """Edits the database; returns [(player, row)] for the players applied."""
    protected = set()
    for p in players.rows:
        if has_portrait(p[ID]):
            protected.add(p[KEY])
    matched = []
    for player in roster:
        row = players.find(player["first_name"], player["last_name"])
        matched.append(row)
        if row is not None:
            protected.add(players.rows[row][KEY])

    def free_agents(position_group):
        for r, p in enumerate(players.rows):
            if (p[NHL_TEAM] == 0 and p[ID] == p[ASSET_ID] and p[ID] in PORTRAIT_IDS
                    and not has_portrait(p[ID]) and p[KEY] not in protected
                    and group(p[POSITION]) == position_group and players.has_ratings(r)):
                yield r

    applied = []
    for player, row in zip(roster, matched):
        team = TEAMS.index(player["team"])
        position = POSITIONS.index(player["position"])
        name = f"{player['first_name']} {player['last_name']}"
        if row is None:
            row = next(free_agents(group(position)), None)
            if row is None:
                raise ValueError(f"{name}: no free {player['position']} record left to use")
            protected.add(players.rows[row][KEY])
            template = players.find(*TEMPLATES[(player.get("type") or "all_around", group(position))].split(" ", 1))
            players.set(row, POSITION, position)
            players.copy_ratings(template, row)
            log(f"{name}: new player (record of {players.rows[row][FIRST]} {players.rows[row][LAST]}, "
                f"id {players.rows[row][ID]})")
        else:
            log(f"{name}: existing player, id {players.rows[row][ID]}")
        p = players.rows[row]
        if p[NHL_TEAM] != team + 1:
            roster_players = [r for r, q in enumerate(players.rows)
                              if q[NHL_TEAM] == team + 1 and q[TEAM] == team
                              and group(q[POSITION]) == group(position) and q[KEY] not in protected
                              and players.has_ratings(r)]
            if roster_players:
                out = min(roster_players, key=players.overall)
                players.set(out, TEAM, 0)
                players.set(out, NHL_TEAM, 0)
                q = players.rows[out]
                protected.add(q[KEY])
                log(f"  {q[FIRST]} {q[LAST]} leaves {player['team']} for free agency")
        players.set(row, FIRST, player["first_name"])
        players.set(row, LAST, player["last_name"])
        players.set(row, TEAM, team)
        players.set(row, NHL_TEAM, team + 1)
        players.set(row, POSITION, position)
        players.set(row, NUMBER, int(player["jersey_number"]))
        if player.get("birth_date"):
            year, month, day = (int(x) for x in player["birth_date"].split("-"))
            players.set(row, BIRTH_YEAR, year - 1900)
            players.set(row, BIRTH_MONTH, month)
            players.set(row, BIRTH_DAY, day)
        if player.get("birthplace"):
            players.set(row, BIRTHPLACE, player["birthplace"])
        applied.append((player, row))
    return applied


# --- Build --------------------------------------------------------------------

def read_text(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except FileNotFoundError:
        return None


def bases(game_dir):
    """(db, big, bh) paths to build from, refreshing the archive backup when
    the game has rebuilt data0.big since it was taken."""
    db, big, bh = (os.path.join(game_dir, n) for n in ("db/nhlng.db", "data0.big", "data0.bh"))
    if not os.path.exists(db + ".orig"):
        shutil.copy2(db, db + ".orig")
    marker = read_text(os.path.join(game_dir, MARKER))
    if marker is None:
        raise SystemExit("Run the game once first: data0.big does not have its front-end mod yet.")
    state = os.path.join(game_dir, STATE)
    if read_text(state) != marker or not os.path.exists(big + ".preroster"):
        shutil.copy2(big, big + ".preroster")
        shutil.copy2(bh, bh + ".preroster")
        with open(state, "w") as f:
            f.write(marker)
    return db + ".orig", big + ".preroster", bh + ".preroster"


def build(game_dir, roster_path, dry_run):
    with open(roster_path) as f:
        roster = json.load(f)["players"]
    for k, player in enumerate(roster):
        validate(player, k)
    db_base, big_base, bh_base = bases(game_dir)
    archive = Archive(bytearray(open(big_base, "rb").read()), open(bh_base, "rb").read())

    def has_portrait(player_id):
        return f"{PORTRAITS[0][0]}/p{player_id}.big" in archive.index

    db = Database.load(db_base)
    players = Players(db)
    roster_dir = os.path.dirname(os.path.abspath(roster_path))
    ready, skipped = [], []
    for player in roster:
        path = os.path.join(roster_dir, player.get("headshot") or "")
        row = players.find(player["first_name"], player["last_name"])
        if os.path.isfile(path) or (row is not None and has_portrait(players.rows[row][ID])):
            ready.append(player)
        else:
            skipped.append(f"{player['first_name']} {player['last_name']}")
    applied = apply_players(players, ready, has_portrait, print)
    for player, row in applied:
        path = os.path.join(roster_dir, player.get("headshot") or "")
        if not os.path.isfile(path):
            continue
        image = xenos_texture.read_png(path)
        player_id = players.rows[row][ID]
        for folder, texture_size, display_size in PORTRAITS:
            archive.put(f"{folder}/p{player_id}.big",
                        portrait_package(archive, folder, texture_size, display_size, image))
        print(f"  portrait p{player_id} from {player['headshot']}")
    if skipped:
        print(f"skipped (no headshot yet): {', '.join(skipped)}")
    if dry_run:
        return
    big, bh = archive.save()
    db.save(os.path.join(game_dir, "db/nhlng.db"))
    with open(os.path.join(game_dir, "data0.big"), "wb") as f:
        f.write(big)
    with open(os.path.join(game_dir, "data0.bh"), "wb") as f:
        f.write(bh)
    print(f"wrote db/nhlng.db, data0.big and data0.bh ({len(applied)} players)")


def restore(game_dir):
    for name in ("db/nhlng.db.orig", "data0.big.preroster", "data0.bh.preroster"):
        path = os.path.join(game_dir, name)
        if os.path.exists(path):
            shutil.copy2(path, os.path.splitext(path)[0])
            print(f"restored {os.path.splitext(path)[0]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir", nargs="?", default=DEFAULT_GAME)
    ap.add_argument("--roster", default=DEFAULT_ROSTER)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--restore", action="store_true")
    args = ap.parse_args()
    if args.restore:
        restore(args.game_dir)
    else:
        build(args.game_dir, args.roster, args.dry_run)


if __name__ == "__main__":
    main()
