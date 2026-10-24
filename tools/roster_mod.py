#!/usr/bin/env python3
"""Adds players to the 3-on-3 arcade switch list from a JSON file of names,
teams, types and headshots (see roster/roster.json). Headshots are
PNG files in roster/headshots/, named by the paths in that JSON. They are
not part of the repo.

The game applies this itself on startup when those PNGs are present. This
script is the same edit, for trying a roster without rebuilding the game.

Usage: roster_mod.py [GAME_DIR] [--roster PATH] [--dry-run] [--restore]

GAME_DIR is the extracted game (default game/extracted). Run the game once
first so its front-end mod is in data0.big; this tool builds on top of it.

What it changes:
  db/nhlng.db       player records (cPbu) and, for new players, ratings
                    copied from a star of the same type and position.
  data0.big / .bh   a large and small portrait per player under
                    fe/ion/artassets/playerheads[small]/.

The Starting Lineups switch list is a fixed pool of the original 43 arcade
stars (the IDs that ship with portraits). New names only appear there when
they take over one of those star slots — appending a portrait on a new ID
is not enough. This tool claims lesser-known star slots for custom players
(or an explicit "replaces" target), and keeps legends unless asked to
replace them. A player joining a team sends that team's lowest-rated player
in the same position group to free agency. Players without a headshot are
skipped unless they already have a portrait.

Every run rebuilds from backups: db/nhlng.db.orig, and data0.big.preroster /
data0.bh.preroster (the archive as the game left it). A newer front-end mod
rebuilds data0.big without the portraits; the next launch of the game puts
them back. --restore puts the backups back.
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
DEFAULT_ROSTER = os.path.join(REPO, "roster", "roster.json")

TEAMS = ["ANA", "ATL", "BOS", "BUF", "CGY", "CAR", "CHI", "COL", "CBJ", "DAL", "DET", "EDM", "FLA",
         "LA", "MIN", "MTL", "NSH", "NJ", "NYI", "NYR", "OTT", "PHI", "PHX", "PIT", "STL", "SJ", "TB",
         "TOR", "VAN", "WSH"]
POSITIONS = ["C", "LW", "RW", "D", "G"]
TYPES = ("all_around", "fast", "strong")

# cPbu (players) fields.
FIRST, LAST, BIRTHPLACE = "PedH", "RMbQ", "JzFM"
ID, ASSET_ID, KEY = "Nzao", "rnOl", "zIBw"
FACE = "DaPp"  # face / portrait asset id; featured stars use FACE == ID
TEAM, NHL_TEAM = "BSXd", "WBbd"  # NHL_TEAM is team + 1, or 0 for a free agent
POSITION, NUMBER, SHOOTS = "aljv", "tRVs", "pkRG"  # SHOOTS: 0 left, 1 right
BIRTH_YEAR, BIRTH_MONTH, BIRTH_DAY = "dnFq", "pLKJ", "iwsK"  # year - 1900
# Height/weight and the fields that travel with type templates (label + build).
BODY = ("PQgy", "WZNs", "jfuZ", "QDTK", "GDhI", "QBpy", "dhKk")
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

# Original arcade stars kept unless a roster entry explicitly replaces them.
KEEP_STARS = {
    "Sidney Crosby", "Alexander Ovechkin", "Evgeni Malkin", "Patrick Kane",
    "Jonathan Toews", "Nicklas Lidstrom", "Zdeno Chara", "Henrik Lundqvist",
    "Martin Brodeur", "Pavel Datsyuk", "Jarome Iginla", "Joe Thornton",
    "Henrik Zetterberg", "Shea Weber", "Anze Kopitar", "Vincent Lecavalier",
    "Ilya Kovalchuk", "Chris Pronger",
}

# Claim order for star slots (least iconic first). Must be original arcade stars.
CLAIM_ORDER = [
    "Nikolai Antropov", "Sean Avery", "Brad Boyes", "Kristian Huselius",
    "Alexander Frolov", "Andrew Cogliano", "Milan Lucic", "Jeremy Roenick",
    "Shane Doan", "Nathan Horton", "Alex Kovalev", "Daniel Briere",
    "Jean-Sebastien Giguere", "Rick DiPietro", "Roberto Luongo",
    "Mike Modano", "Joe Sakic", "Dion Phaneuf", "Mike Green", "Thomas Vanek",
    "Saku Koivu", "Jason Spezza", "Eric Staal", "Dany Heatley", "Marian Gaborik",
]


def normalize(name):
    decomposed = unicodedata.normalize("NFKD", name)
    return "".join(c for c in decomposed if not unicodedata.combining(c)).casefold().strip()


def group(position):
    return "G" if position == 4 else "D" if position == 3 else "F"


def full_name(first, last):
    return f"{first} {last}"


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

    def put(self, name, data, unpacked=0):
        if name in self.index:
            self.index[name][3] = unpacked
            self.index[name][4] = data
        else:
            entry = [name, 0, 0, unpacked, data]
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
                e[1], e[2] = len(big), len(e[4])
                # e[3] (unpacked) kept from put(); 0 means stored uncompressed.
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
    """Portrait .big from the template player's, with new pixels.

    Stored uncompressed (bh unpacked size 0), same as the front-end screen
    patches. A literal-only RefPack blob is larger than its payload, and the
    game's loader rejects that (null texture -> crash in the switch list).
    """
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

    def copy_body(self, source, target):
        for field in BODY:
            self.set(target, field, self.rows[source][field])


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


def star_ids(has_portrait):
    """Player IDs that already have portraits in the base archive (the arcade pool)."""
    return {pid for pid in PORTRAIT_IDS if has_portrait(pid)}


def apply_players(players, roster, has_portrait, log):
    """Edits the database; returns [(player, row)] for the players applied.

    Custom players are bound to original arcade-star IDs so they appear in the
    Starting Lineups switch list. Lesser-known stars are claimed first unless
    "replaces" names a specific player.
    """
    stars = star_ids(has_portrait)
    protected = set()
    keep = {normalize(n) for n in KEEP_STARS}
    for player in roster:
        keep.add(normalize(full_name(player["first_name"], player["last_name"])))
        if player.get("replaces"):
            keep.add(normalize(player["replaces"]))

    def row_name(r):
        p = players.rows[r]
        return full_name(p[FIRST], p[LAST])

    def find_star_row(name):
        first, _, last = name.partition(" ")
        row = players.find(first, last)
        if row is not None and players.rows[row][ID] in stars:
            return row
        # Some stars have multi-word first names (Jean-Sebastien).
        for r, p in enumerate(players.rows):
            if p[ID] in stars and normalize(row_name(r)) == normalize(name):
                return r
        return None

    # Star rows we may overwrite, in claim order then any remaining non-kept.
    # Keep skaters and goalies separate — rating tables differ by position group.
    claimable = []
    seen = set()
    for name in CLAIM_ORDER:
        row = find_star_row(name)
        if row is None or players.rows[row][KEY] in seen:
            continue
        if normalize(name) in keep:
            continue
        claimable.append(row)
        seen.add(players.rows[row][KEY])
    for r, p in enumerate(players.rows):
        if p[ID] not in stars or p[KEY] in seen:
            continue
        if normalize(row_name(r)) in keep:
            continue
        claimable.append(r)
        seen.add(p[KEY])

    def claim_slot(position_group):
        for i, row in enumerate(claimable):
            if group(players.rows[row][POSITION]) != position_group:
                continue
            claimable.pop(i)
            return row
        return None

    applied = []
    for player in roster:
        team = TEAMS.index(player["team"])
        position = POSITIONS.index(player["position"])
        name = full_name(player["first_name"], player["last_name"])
        existing = players.find(player["first_name"], player["last_name"])

        if player.get("replaces"):
            row = find_star_row(player["replaces"])
            if row is None:
                raise ValueError(f"{name}: replaces {player['replaces']!r} but that arcade star was not found")
            log(f"{name}: replaces {player['replaces']} (id {players.rows[row][ID]})")
        elif existing is not None and players.rows[existing][ID] in stars:
            row = existing
            log(f"{name}: existing arcade star, id {players.rows[row][ID]}")
        else:
            row = claim_slot(group(position))
            if row is None:
                log(f"{name}: skipped, no arcade {player['position']} star slot left to claim")
                continue
            old = row_name(row)
            log(f"{name}: takes arcade slot of {old} (id {players.rows[row][ID]})")
            if existing is not None and existing != row:
                # Park the displaced star identity on the old non-star row, free agent.
                disp_first = players.rows[row][FIRST]
                disp_last = players.rows[row][LAST]
                players.set(existing, FIRST, disp_first)
                players.set(existing, LAST, disp_last)
                players.set(existing, TEAM, 0)
                players.set(existing, NHL_TEAM, 0)
                protected.add(players.rows[existing][KEY])
                log(f"  {disp_first} {disp_last} moved to free agency (was id {players.rows[existing][ID]})")

        protected.add(players.rows[row][KEY])
        template = players.find(*TEMPLATES[(player.get("type") or "all_around", group(position))].split(" ", 1))
        # Always take the JSON type's ratings and build, including players who
        # were already arcade stars (Forsberg, Selanne, …).
        players.set(row, POSITION, position)
        if template is not None:
            players.copy_ratings(template, row)
            players.copy_body(template, row)

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
        shoots = player.get("shoots")
        if shoots:
            side = str(shoots).casefold()
            if side not in ("left", "right", "l", "r"):
                raise ValueError(f"{name}: shoots must be left or right")
            players.set(row, SHOOTS, 0 if side in ("left", "l") else 1)
        players.set(row, FACE, players.rows[row][ID])
        players.set(row, ASSET_ID, players.rows[row][ID])
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
                        portrait_package(archive, folder, texture_size, display_size, image),
                        unpacked=0)
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
