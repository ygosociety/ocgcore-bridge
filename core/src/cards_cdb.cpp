// SPDX-License-Identifier: AGPL-3.0-or-later
// BabelCDB / EDOPro card databases: SQLite files with a `datas` table
// (id, ot, alias, setcode, type, atk, def, level, race, attribute, category).
#include "ocgbridge/cards.h"

#include <cstdio>

#ifdef OCGBRIDGE_WITH_SQLITE
#include <sqlite3.h>
#endif

namespace ocgbridge {

bool CardDatabase::cdb_supported() {
#ifdef OCGBRIDGE_WITH_SQLITE
    return true;
#else
    return false;
#endif
}

long CardDatabase::load_cdb(const std::string& path, std::string* error) {
#ifndef OCGBRIDGE_WITH_SQLITE
    if (error) *error = "built without SQLite: .cdb card databases are not supported";
    (void)path;
    return -1;
#else
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (error) *error = "cannot open " + path + ": " + (db ? sqlite3_errmsg(db) : "out of memory");
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id, alias, setcode, type, atk, def, level, race, attribute FROM datas";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (error) *error = std::string("not a card database: ") + sqlite3_errmsg(db);
        sqlite3_close(db);
        return -1;
    }
    long added = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        CardEntry e;
        e.code = static_cast<uint32_t>(sqlite3_column_int64(st, 0));
        if (!e.code) continue;
        e.alias = static_cast<uint32_t>(sqlite3_column_int64(st, 1));
        const uint64_t packed = static_cast<uint64_t>(sqlite3_column_int64(st, 2));
        for (int i = 0; i < 4; ++i) {
            const uint16_t s = static_cast<uint16_t>((packed >> (16 * i)) & 0xFFFF);
            if (s) e.setcodes.push_back(s);
        }
        e.type = static_cast<uint32_t>(sqlite3_column_int64(st, 3));
        e.attack = static_cast<int32_t>(sqlite3_column_int64(st, 4));
        e.defense = static_cast<int32_t>(sqlite3_column_int64(st, 5));
        const uint32_t level = static_cast<uint32_t>(sqlite3_column_int64(st, 6));
        // EDOPro's layout: level in the low byte, the Pendulum Scales above it.
        e.level = level & 0xFF;
        e.lscale = (level >> 24) & 0xFF;
        e.rscale = (level >> 16) & 0xFF;
        e.pack_scales = false;
        e.race = static_cast<uint64_t>(sqlite3_column_int64(st, 7));
        e.attribute = static_cast<uint32_t>(sqlite3_column_int64(st, 8));
        // A Link monster's arrows are stored in `def`.
        if (e.type & 0x4000000) {
            e.link_marker = static_cast<uint32_t>(e.defense);
            e.defense = 0;
        }
        if (cards_.emplace(e.code, std::move(e)).second) added++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    std::fprintf(stderr, "[ocgcore-bridge] %s: %ld cards\n", path.c_str(), added);
    return added;
#endif
}

} // namespace ocgbridge
