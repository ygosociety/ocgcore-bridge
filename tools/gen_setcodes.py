#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Generates data/setcodes.json: every card's real archetype setcodes and alias.

cards.json (YGOPRODeck) carries one archetype *name* per card, never the numeric
setcodes the card scripts check with Card:IsSetCard, so sub-archetypes
("Sky Striker Ace" 0x1115 under "Sky Striker" 0x115), cards in several
archetypes and archetype names that don't normalize onto a SET_ constant had
no setcode at all. EDOPro's own database (ProjectIgnis/BabelCDB, the data these
scripts are written against) has them: `datas.setcode` packs up to four 16-bit
setcodes into one 64-bit integer. It also has `datas.alias`, which cards.json
lacks: "This card is always treated as X" (Fallen of the White Dragon as
Fallen of Albaz) and artworks of one card; every non-zero alias is written.
And its tokens ("tokens": code -> [type, atk, def, level, race, attribute], the
database's own values): cards.json lists few of them, and a script can only
Special Summon a token the engine has data for (Nibiru's Primal Being Token).

    python3 tools/gen_setcodes.py <BabelCDB dir> <BabelCDB commit> [cards.json] > setcodes.json

With cards.json, a card it lists under a code no database has (a new TCG
passcode, e.g. Angelechy Opposition 86457955, that EDOPro still keeps under
its pre-release id 101402094 and pre-release/c101402094.lua) is matched by
exact name: its alias becomes the database's id (so the bridge loads that
script and IsCode matches the id other scripts list) and it takes its setcodes.

<BabelCDB dir> holds cards.cdb and optionally release-*.cdb / prerelease-*.cdb
(Rush and skill databases are not read). A card is taken from the first
database that has it: cards.cdb, then release-*, then prerelease-*.
"""
import glob
import json
import os
import sqlite3
import sys


def unpack(packed: int) -> list[int]:
    out = []
    for i in range(4):
        code = (packed >> (16 * i)) & 0xFFFF
        if code:
            out.append(code)
    return out


def main() -> None:
    src, commit = sys.argv[1], sys.argv[2]
    cards_json = sys.argv[3] if len(sys.argv) > 3 else None
    dbs = [os.path.join(src, "cards.cdb")]
    dbs += sorted(glob.glob(os.path.join(src, "release-*.cdb")))
    dbs += sorted(p for p in glob.glob(os.path.join(src, "prerelease-*.cdb")) if "rush" not in os.path.basename(p))
    setcodes: dict[str, list[int]] = {}
    aliases: dict[str, int] = {}
    seen: set[str] = set()
    by_name: dict[str, tuple[int, list[int]]] = {}
    # names only an aliased row carries (a pre-release card that is an alias of its final
    # printing but is the one with a script under its own id): a fallback to by_name
    by_name_aliased: dict[str, tuple[int, list[int]]] = {}
    tokens: dict[str, list[int]] = {}
    for db in dbs:
        con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
        for row in con.execute("SELECT id, type, atk, def, level, race, attribute FROM datas WHERE type & 0x4000 != 0"):
            tokens.setdefault(str(row[0]), [int(row[1]), int(row[2]), int(row[3]), int(row[4]) & 0xFF, int(row[5]), int(row[6])])
        for card_id, packed, name, alias in con.execute("SELECT d.id, d.setcode, t.name, d.alias FROM datas d JOIN texts t ON t.id = d.id"):
            (by_name if not alias else by_name_aliased).setdefault(name, (int(card_id), unpack(packed or 0)))
        for card_id, packed, alias in con.execute("SELECT id, setcode, alias FROM datas"):
            key = str(card_id)
            if key in seen:
                continue
            seen.add(key)
            codes = unpack(packed or 0)
            if codes:
                setcodes[key] = codes
            if alias:
                aliases[key] = int(alias)
        con.close()
    renamed: dict[str, int] = {}
    if cards_json:
        with open(cards_json) as f:
            listed = json.load(f)
        for card in listed["data"] if isinstance(listed, dict) else listed:
            key = str(card["id"])
            name = card.get("name")
            if key in seen or (name not in by_name and name not in by_name_aliased):
                continue
            db_id, codes = by_name.get(name) or by_name_aliased[name]
            if db_id == card["id"]:
                continue
            aliases[key] = db_id
            renamed[key] = db_id
            if codes:
                setcodes[key] = codes
    out = {
        "source": f"ProjectIgnis/BabelCDB@{commit}",
        "databases": [os.path.basename(p) for p in dbs],
        "setcodes": dict(sorted(setcodes.items(), key=lambda kv: int(kv[0]))),
        "aliases": dict(sorted(aliases.items(), key=lambda kv: int(kv[0]))),
        # cards.json codes no database has, mapped by name (a subset of aliases)
        "byName": dict(sorted(renamed.items(), key=lambda kv: int(kv[0]))),
        "tokens": dict(sorted(tokens.items(), key=lambda kv: int(kv[0]))),
    }
    json.dump(out, sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
