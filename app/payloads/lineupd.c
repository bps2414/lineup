// SPDX-License-Identifier: GPL-3.0-or-later
//
// lineupd: a small HTTP service that reads the PS5 home-screen rows and saves a new order.
// A row is the rows of /system_data/priv/mms/app.db table tbl_iconinfo_<userId> with visible = 1 and
// dispLocation 138 (the Games tab) or 188 (Media), shown by lastAccessIndex, largest first. Saving gives the
// posted order the indices those rows already hold, so no index grows past the counter the home
// screen keeps in memory. Writes go through SQLite in place, inside one transaction, after a backup.
//
// Endpoints (JSON unless noted), on 127.0.0.1:9077 (0.0.0.0 when built with -DLINEUPD_LAN):
//   GET  /version                {"version","build"}
//   GET  /profiles                 {"profiles":[{"userId","hex","name","avatar"}]}
//   GET  /row?user=<10 dig>[&tab=games|media]  {"user","tab","pins":"on"|"missing"|"off","games":[{"titleId",
//                                "name","index","icon","pinned":slot|null}]}; games is the Games tab
//                                (dispLocation 138), media the Media tab (188)
//   GET  /icon/<titleId>         image/png
//   GET  /avatar/<10 dig>        image/png, the profile's picture
//   POST /row?user=<10 dig>[&tab=games|media]  X-Lineup: 1; body "LIN1\n" then "<titleId> <index>[ pin]\n" per
//                                app, in the new order, with the index the client read; " pin" pins it at that place
//   POST /pins/off?user=<10 dig> X-Lineup: 1; unpins that profile's games
//   POST /pins/uninstall         X-Lineup: 1; removes every pin, trigger and table Lineup added to app.db and appinfo.db
//   GET  /hidden?user=<10 dig>   {"hidden":[{"titleId","name"}],"system":[{"titleId","tab"}]}, what Lineup hid on
//                                that profile, and the system tiles showing on a tab, the PS Store, the Media
//                                Gallery, App Library and Welcome with their tabs (games, media or none)
//   POST /app?user=<10 dig>&title=<id>&action=hide|show|games|media  X-Lineup: 1; hides or shows an app on that
//                                profile, or moves it to that tab on every profile
//                                The PS Store and the Media Gallery move too (move_system); not the libraries
//   POST /tabshifter/remove      X-Lineup: 1; {"ok","removed","folders","backup"}, takes TabShifter's TBSH00001 tile
//                                out: removed counts the rows, folders the folders that were there and went
//   POST /report                 X-Lineup: 1; writes /data/lineup/report.json, the Games and Media tabs' apps
//   POST /quit                   X-Lineup: 1; exits after answering
//   GET  /update/install         X-Lineup: 1; {"kind":"folder"|"image","path","previous"}
//   POST /update/apply?version=<x.y.z|previous>  X-Lineup: 1; backs up the installed folder first and answers
//                                when that is done; then waits for the app to close, swaps its files for the
//                                downloaded ones (or the previous ones), starts the installed lineupd.elf and opens the app
//
// Built with -DLINEUPD_DIAG (port 9078, LAN), also:
//   GET    /diag                 avatars, homes, mounts, install candidates, SQLite version and strings
//   POST   /diag/trace?user=<10 dig>  X-Lineup: 1; backup, then log-only triggers on tbl_iconinfo_<user>
//   GET    /diag/trace           the trace rows and the triggers in place
//   DELETE /diag/trace           X-Lineup: 1; drops every lineup_trace trigger and the table
//
// Built with -DLINEUPD_TEST it compiles on a desktop and runs a self-check against a copy of app.db,
// or a synthetic database without arguments.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "sqlite3.h"
#ifndef LINEUPD_TEST
#include <sys/param.h>
#include <sys/mount.h>
#endif

#ifdef LINEUPD_TEST
#include <assert.h>
#define LOG(...) (fprintf(stderr, "[lineupd] " __VA_ARGS__), fputc('\n', stderr))
#else
#include <ps5/klog.h>
#define LOG(fmt, ...) klog_printf("[lineupd] " fmt "\n", ##__VA_ARGS__)
#endif

#define LINEUPD_VERSION "2.0.1"
// Which build this is: "local", or the commit for a CI build (.github/workflows/ci.yml writes it, with BUILD in
// app/api.js). Builds of one version answer /version alike, so without it the app kept talking to a lineupd left
// running by an earlier test build.
#define LINEUPD_BUILD "local"
#define TITLE_ID "PPSA99077"
#ifdef LINEUPD_DIAG
// Phase 0 diagnostic build (docs/pt-BR/PLANO-1.2.md): its own port, open to the LAN, so it runs beside the
// app's lineupd and still answers when the home screen does not.
#define PORT 9078
#define LINEUPD_LAN
#else
#define PORT 9077
#endif
#define MAX_HEADER (16 * 1024)
#define MAX_BODY (64 * 1024)
#define MAX_GAMES 512
// The home screen shows this many games of the row, as HOME_LIMIT in app/index.jsx.
#define HOME_LIMIT 12
// And this many apps of the Media tab, as MEDIA_HOME_LIMIT in app/index.jsx.
#define MEDIA_HOME_LIMIT 11
#define KEEP_BACKUPS 10

static const char* db_path = "/system_data/priv/mms/app.db";
static const char* appinfo_path = "/system_data/priv/mms/appinfo.db";
static const char* backup_dir = "/data/lineup/backups";
static const char* home_dir = "/user/home";
static const char* appmeta_dir = "/user/appmeta";
static const char* profile_cache_dir = "/system_data/priv/cache/profile";
static const char* update_dir = "/data/lineup/update";

// ---- Growable text buffer with JSON escaping ----

typedef struct { char* data; size_t size, capacity; } Buffer;

static void put(Buffer* b, const char* text, size_t size) {
  if (b->size + size + 1 > b->capacity) {
    size_t capacity = b->capacity ? b->capacity : 1024;
    while (b->size + size + 1 > capacity) capacity *= 2;
    char* data = realloc(b->data, capacity);
    if (!data) return;
    b->data = data;
    b->capacity = capacity;
  }
  memcpy(b->data + b->size, text, size);
  b->size += size;
  b->data[b->size] = 0;
}

static void puts_(Buffer* b, const char* text) { put(b, text, strlen(text)); }

static void putf(Buffer* b, const char* fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  int size = vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  if (size > 0) put(b, line, (size_t)size < sizeof line ? (size_t)size : sizeof line - 1);
}

static void put_json_string(Buffer* b, const char* text) {
  puts_(b, "\"");
  for (const unsigned char* c = (const unsigned char*)(text ? text : ""); *c; ++c) {
    if (*c == '"' || *c == '\\') { char escaped[2] = {'\\', (char)*c}; put(b, escaped, 2); }
    else if (*c < 0x20) putf(b, "\\u%04x", *c);
    else put(b, (const char*)c, 1);
  }
  puts_(b, "\"");
}

// ---- Validation ----

static int is_user_id(const char* id) {
  if (!id || strlen(id) != 10) return 0;
  for (int i = 0; i < 10; ++i) if (!isdigit((unsigned char)id[i])) return 0;
  return 1;
}

static int is_title_id(const char* id) {
  if (!id || strlen(id) != 9) return 0;
  for (int i = 0; i < 4; ++i) if (id[i] < 'A' || id[i] > 'Z') return 0;
  for (int i = 4; i < 9; ++i) if (!isdigit((unsigned char)id[i])) return 0;
  return 1;
}

// ---- Reorder logic (pure) ----

typedef struct { char title[16]; long long index; int pinned; } Row;

static int by_index_descending(const void* a, const void* b) {
  long long x = *(const long long*)a, y = *(const long long*)b;
  return x < y ? 1 : x > y ? -1 : 0;
}

// Fills `out` with the index each posted game gets: the current rows' indices, largest first, in the
// posted order. Fails when the posted list is not exactly the current rows, or when they no longer stand in the
// order read. Only the order counts: while Lineup is open the console raises lastAccessIndex every half minute or
// so, and lineup_pin_u renumbers the pinned apps, so the indices read go stale while the row stays the same.
static int plan_reorder(const Row* current, int count, const Row* posted, int posted_count, long long* out) {
  static int at[MAX_GAMES];
  if (count != posted_count || count <= 0 || count > MAX_GAMES) return -1;
  for (int i = 0; i < posted_count; ++i) {
    at[i] = -1;
    for (int j = 0; j < count; ++j) if (!strcmp(posted[i].title, current[j].title)) { at[i] = j; break; }
    if (at[i] < 0) return -1;
    for (int j = 0; j < i; ++j) if (!strcmp(posted[i].title, posted[j].title)) return -1;
  }
  // Read before another, an app must still be ahead of it.
  for (int i = 0; i < posted_count; ++i)
    for (int j = 0; j < posted_count; ++j)
      if (posted[i].index > posted[j].index && current[at[i]].index < current[at[j]].index) return -1;
  for (int i = 0; i < count; ++i) out[i] = current[i].index;
  qsort(out, (size_t)count, sizeof *out, by_index_descending);
  return 0;
}

// Parses "LIN1\n" + "<titleId> <index>[ pin]\n" lines. Returns the number of rows, or -1.
static int parse_order(const char* body, Row* rows, int capacity) {
  if (strncmp(body, "LIN1\n", 5)) return -1;
  int count = 0;
  for (const char* line = body + 5; *line;) {
    const char* end = strchr(line, '\n');
    size_t length = end ? (size_t)(end - line) : strlen(line);
    if (length) {
      char text[64];
      if (length >= sizeof text || count == capacity) return -1;
      memcpy(text, line, length);
      text[length] = 0;
      char title[16], flag[8], extra;
      long long index;
      int fields = sscanf(text, "%15s %lld %7s %c", title, &index, flag, &extra);
      if (fields < 2 || fields > 3 || (fields == 3 && strcmp(flag, "pin")) || !is_title_id(title)) return -1;
      strcpy(rows[count].title, title);
      rows[count].pinned = fields == 3;
      rows[count++].index = index;
    }
    if (!end) break;
    line = end + 1;
  }
  return count;
}

// ---- Database ----

static sqlite3* open_db(int writable) {
  sqlite3* db = NULL;
  if (sqlite3_open_v2(db_path, &db, writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    LOG("open %s: %s", db_path, db ? sqlite3_errmsg(db) : "no memory");
    sqlite3_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db, 5000);
  return db;
}

// Whether the schema has a table or trigger named `name`.
static int has_object(sqlite3* db, const char* name) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name=?", -1, &statement, NULL)) return 0;
  sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC);
  int exists = sqlite3_step(statement) == SQLITE_ROW;
  sqlite3_finalize(statement);
  return exists;
}

static int table_exists(sqlite3* db, const char* user) {
  char name[64];
  snprintf(name, sizeof name, "tbl_iconinfo_%s", user);
  return has_object(db, name);
}

#define GAMES_TAB 138
#define MEDIA_TAB 188

// The tab a row of tbl_iconinfo_<user> is on, for the row Lineup shows: 138 or 188 as stored, and the Media Gallery
// (NPXS40075), a system tile, by the tab bit of its own value (0x02 Games, 0x04 Media; see Moving system tiles). The
// home screen orders it with the apps: opening it raises its lastAccessIndex as it does theirs. NULL off both rows.
// `p` is NEW., OLD. or empty.
#define TAB_OF(p) "(CASE WHEN " p "dispLocation IN (138, 188) THEN " p "dispLocation WHEN " p "titleId = 'NPXS40075' " \
  "THEN CASE WHEN " p "dispLocation & 4 THEN 188 WHEN " p "dispLocation & 2 THEN 138 END END)"

// The apps on tab `location` (GAMES_TAB or MEDIA_TAB), in home order. `user` must already be validated: table
// names cannot be bound.
static int read_tab(sqlite3* db, const char* user, int location, Row* rows, char (*names)[256], int capacity) {
  char sql[512];
  snprintf(sql, sizeof sql, "SELECT titleId, titleName, lastAccessIndex FROM tbl_iconinfo_%s "
           "WHERE visible = 1 AND " TAB_OF("") " = %d ORDER BY lastAccessIndex DESC", user, location);
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
  int count = 0;
  while (count < capacity && sqlite3_step(statement) == SQLITE_ROW) {
    const char* title = (const char*)sqlite3_column_text(statement, 0);
    const char* name = (const char*)sqlite3_column_text(statement, 1);
    snprintf(rows[count].title, sizeof rows[count].title, "%s", title ? title : "");
    if (names) snprintf(names[count], sizeof names[count], "%s", name && *name ? name : rows[count].title);
    rows[count].pinned = 0;
    rows[count++].index = sqlite3_column_int64(statement, 2);
  }
  sqlite3_finalize(statement);
  return count;
}

// Only homebrew and emulators installed under /user/app change tabs, Lineup too: the system's apps, games and PS4
// titles stay out.
static int can_move(const char* title, int type, int place) {
  // The system's own tiles have appType 0 and contentLocation 2 too; the PS Store and the Media Gallery move
  // through move_system instead.
  return (type == 0 || type == 5) && place == 2 && strncmp(title, "CUSA", 4) && strncmp(title, "NPXS", 4);
}

// Whether the row should offer moving `title`. move_app checks again, with the rest of what it needs.
static int is_movable(sqlite3* db, const char* title) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT appType, contentLocation FROM tbl_contentinfo WHERE titleId = ?", -1, &statement,
                         NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  int movable = sqlite3_step(statement) == SQLITE_ROW &&
                can_move(title, sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1));
  sqlite3_finalize(statement);
  return movable;
}

// The icon the home screen draws: tbl_contentinfo.icon0Info (/user/app/<id>/sce_sys/icon0.png?ts=...),
// else /user/appmeta/<id>/icon0.png. `title` must already be validated.
static void icon_path(sqlite3* db, const char* title, char* out, size_t size) {
  snprintf(out, size, "%s/%s/icon0.png", appmeta_dir, title);
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT icon0Info FROM tbl_contentinfo WHERE titleId = ?", -1, &statement, NULL)) return;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  if (sqlite3_step(statement) == SQLITE_ROW) {
    const char* info = (const char*)sqlite3_column_text(statement, 0);
    size_t length = info ? strcspn(info, "?") : 0;
    if (length && length < size && !strncmp(info, "/user/", 6) && !strstr(info, "..")) {
      memcpy(out, info, length);
      out[length] = 0;
    }
  }
  sqlite3_finalize(statement);
}

static int pragma_text(sqlite3* db, const char* sql, char* out, size_t size) {
  sqlite3_stmt* statement;
  snprintf(out, size, "failed");
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(statement) == SQLITE_ROW) snprintf(out, size, "%s", (const char*)sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
  return strcmp(out, "ok") ? -1 : 0;
}

static int names_ascending(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

// Keeps the newest KEEP_BACKUPS app-*.db backups, each with its appinfo.db copy beside it (app-<time>-<n>.appinfo.db);
// names carry a zero-padded time, so they sort by age. original-*.db, the copies from the first start, never go.
static void rotate_backups(void) {
  DIR* dir = opendir(backup_dir);
  if (!dir) return;
  char* names[256];
  int count = 0;
  for (struct dirent* entry; (entry = readdir(dir)) && count < 256;) {
    size_t length = strlen(entry->d_name);
    if (!strncmp(entry->d_name, "app-", 4) && length > 3 && !strcmp(entry->d_name + length - 3, ".db") &&
        !strstr(entry->d_name, ".appinfo."))
      names[count++] = strdup(entry->d_name);
  }
  closedir(dir);
  qsort(names, (size_t)count, sizeof *names, names_ascending);
  for (int i = 0; i < count; ++i) {
    if (i < count - KEEP_BACKUPS) {
      char path[512];
      snprintf(path, sizeof path, "%s/%s", backup_dir, names[i]);
      unlink(path);
      snprintf(path, sizeof path, "%s/%.*s.appinfo.db", backup_dir, (int)strlen(names[i]) - 3, names[i]);
      unlink(path);
    }
    free(names[i]);
  }
}

static void make_dirs(const char* path) {
  char partial[512];
  snprintf(partial, sizeof partial, "%s", path);
  for (char* slash = partial + 1; *slash; ++slash)
    if (*slash == '/') { *slash = 0; mkdir(partial, 0777); *slash = '/'; }
  mkdir(partial, 0777);
}

// Checks the database and copies it to a new file in backup_dir. Returns 0, or -1 with `message` set.
static int check_and_backup(sqlite3* db, char* backup, size_t backup_size, char* message, size_t message_size) {
  char check[128];
  if (pragma_text(db, "PRAGMA quick_check", check, sizeof check)) {
    snprintf(message, message_size, "The home screen database failed its check (%s). Nothing was saved.", check);
    return -1;
  }
  make_dirs(backup_dir);
  // A new name each time: Lineup's checks at start can back up several times in the same second, and the first of
  // those copies is the one taken before any of them changed anything.
  // The number after the highest taken, not the first free: rotation may have removed a lower one, and a name sorts
  // as old as its number.
  long long now = (long long)time(NULL);
  struct stat taken;
  int next = 0;
  for (int n = 99; n >= 0 && !next; --n) {
    snprintf(backup, backup_size, "%s/app-%010lld-%02d.db", backup_dir, now, n);
    if (!stat(backup, &taken)) next = n + 1;
  }
  snprintf(backup, backup_size, "%s/app-%010lld-%02d.db", backup_dir, now, next);
  if (next > 99) {
    snprintf(message, message_size, "Too many backups this second. Nothing was saved.");
    return -1;
  }
  char sql[600];
  snprintf(sql, sizeof sql, "VACUUM INTO '%s'", backup);
  if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The backup failed (%s). Nothing was saved.", sqlite3_errmsg(db));
    return -1;
  }
  rotate_backups();
  return 0;
}

// Copies the database at `from` to `to` once: if `to` is there already, it is kept as it is. Returns 0 or -1.
static int copy_once(const char* from, const char* to) {
  struct stat info;
  if (!stat(to, &info)) return 0;
  sqlite3* db = NULL;
  int failed = sqlite3_open_v2(from, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK;
  if (!failed) sqlite3_busy_timeout(db, 5000);
  char partial[300], sql[700];
  snprintf(partial, sizeof partial, "%s.partial", to);
  unlink(partial);
  snprintf(sql, sizeof sql, "VACUUM INTO '%s'", partial);
  failed = failed || sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK || rename(partial, to);
  if (failed) {
    LOG("could not copy %s to %s: %s", from, to, db ? sqlite3_errmsg(db) : "no memory");
    unlink(partial);
  }
  sqlite3_close(db);
  return failed ? -1 : 0;
}

// The first time this lineupd starts, before it changes anything: both databases as they were, kept for good
// (rotate_backups leaves them alone). Returns 0, or -1 when a copy failed.
static int backup_originals(void) {
  make_dirs(backup_dir);
  char app[300], info[300];
  snprintf(app, sizeof app, "%s/original-app.db", backup_dir);
  snprintf(info, sizeof info, "%s/original-appinfo.db", backup_dir);
  int failed = copy_once(db_path, app);
  failed = copy_once(appinfo_path, info) || failed;
  return failed ? -1 : 0;
}

// Whether `schema` (main or info) has a table or trigger named `name`.
static int has_object_in(sqlite3* db, const char* schema, const char* name) {
  char sql[96];
  snprintf(sql, sizeof sql, "SELECT 1 FROM %s.sqlite_master WHERE name = ?", schema);
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC);
  int exists = sqlite3_step(statement) == SQLITE_ROW;
  sqlite3_finalize(statement);
  return exists;
}

// Whether `schema`.`table` has a column named `column`: a firmware may not have it, and a trigger or update naming a
// column that is not there fails, inside the system's own writes for a trigger.
static int has_column(sqlite3* db, const char* schema, const char* table, const char* column) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info(?1, ?2) WHERE name = ?3", -1, &statement, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(statement, 1, table, -1, SQLITE_STATIC);
  sqlite3_bind_text(statement, 2, schema, -1, SQLITE_STATIC);
  sqlite3_bind_text(statement, 3, column, -1, SQLITE_STATIC);
  int exists = sqlite3_step(statement) == SQLITE_ROW;
  sqlite3_finalize(statement);
  return exists;
}

// ---- Pinned games ----
//
// A pinned app keeps its place when the system moves apps: three triggers on tbl_iconinfo_<user>
// (lineup_pin_u_, _i_ and _d_<user>) run REORDER inside the system's own write, so the home screen rereads a tab
// already in the pinned order. The Games and Media tabs keep their pins apart: REORDER moves only the tab of the
// app that fired it, and an app is on one tab, so its slot is a place on that tab. lineup_pin holds the slots, lineup_seq is 0..MAX_GAMES-1 (to find the k-th free
// place) and lineup_tmp is REORDER's scratch table, empty between writes.

#define PINNED "SELECT titleId FROM lineup_pin WHERE user = '%1$s'"

// In SQL the home screen's SQLite reads: no CTEs (not allowed in triggers anyway), window functions, UPSERT or
// RETURNING. %1$s is the user and `row` NEW. or OLD., the app that fired it. In lineup_tmp, v is a game's index, r the place it gets and nv that place's index
// (before that, for a game that is not pinned, how many of those were used after it).
// ponytail: places stop at MAX_GAMES; a game past them gets no place and keeps its index.
#define REORDER(row) \
  /* The tab as it is now. */ \
  "INSERT OR IGNORE INTO lineup_tmp (titleId, v) SELECT titleId, lastAccessIndex FROM tbl_iconinfo_%1$s " \
  "WHERE visible = 1 AND " TAB_OF("") " = " TAB_OF(row) ";" \
  /* A pinned game keeps its slot; pinned past the end of a shorter row, they pack at its end in slot order. */ \
  "UPDATE lineup_tmp SET r = (SELECT MIN(p.slot, (SELECT COUNT(*) FROM lineup_tmp) - 1 - (SELECT COUNT(*) " \
  "FROM lineup_pin q, lineup_tmp t WHERE q.user = p.user AND t.titleId = q.titleId AND q.slot > p.slot)) " \
  "FROM lineup_pin p WHERE p.user = '%1$s' AND p.titleId = lineup_tmp.titleId) WHERE titleId IN (" PINNED ");" \
  /* The others take the free places by how recently they were used, as the system orders them. */ \
  "UPDATE lineup_tmp SET nv = (SELECT COUNT(*) FROM lineup_tmp o WHERE o.titleId NOT IN (" PINNED ") AND " \
  "(o.v > lineup_tmp.v OR (o.v = lineup_tmp.v AND o.titleId < lineup_tmp.titleId))) WHERE titleId NOT IN (" PINNED ");" \
  "UPDATE lineup_tmp SET r = (SELECT MIN(s.i) FROM lineup_seq s WHERE s.i < (SELECT COUNT(*) FROM lineup_tmp) AND " \
  "s.i NOT IN (SELECT t.r FROM lineup_tmp t WHERE t.titleId IN (" PINNED ")) AND s.i - (SELECT COUNT(*) " \
  "FROM lineup_tmp t WHERE t.titleId IN (" PINNED ") AND t.r < s.i) = lineup_tmp.nv) WHERE titleId NOT IN (" PINNED ");" \
  /* Place r gets the r-th largest index the row already has, as in save_order. */ \
  "UPDATE lineup_tmp SET nv = (SELECT a.v FROM lineup_tmp a WHERE (SELECT COUNT(*) FROM lineup_tmp b " \
  "WHERE b.v > a.v OR (b.v = a.v AND b.titleId < a.titleId)) = lineup_tmp.r);" \
  "UPDATE tbl_iconinfo_%1$s SET lastAccessIndex = (SELECT t.nv FROM lineup_tmp t WHERE t.titleId = " \
  "tbl_iconinfo_%1$s.titleId) WHERE visible = 1 AND " TAB_OF("") " = " TAB_OF(row) " AND titleId IN " \
  "(SELECT titleId FROM lineup_tmp WHERE nv <> v);" \
  "DELETE FROM lineup_tmp;"

// Opening an app raises its index (twice, a few seconds apart); installing inserts and uninstalling deletes a row.
// Only the apps on the Games (dispLocation 138) and Media (188) tabs fire them. recursive_triggers is off by default, so REORDER's own UPDATE does not fire them again; a
// non-empty lineup_tmp stops them too, should it be on.
#define PIN_TRIGGER(kind, event, when, row) \
  "CREATE TRIGGER lineup_pin_" kind "_%1$s AFTER " event " ON tbl_iconinfo_%1$s WHEN " when row "visible = 1 AND " \
  TAB_OF(row) " " PIN_TABS " (SELECT 1 FROM lineup_tmp) BEGIN " REORDER(row) " END;"
// Triggers without this text are older: 1.4.2's moved the Games tab whatever app fired them, 1.4.3's ran for the
// Games tab only, and 1.8.0's left out the Media Gallery. update_pin_triggers replaces them.
#define PIN_TABS "IS NOT NULL AND NOT EXISTS"
#define PIN_TRIGGERS \
  PIN_TRIGGER("u", "UPDATE OF lastAccessIndex", "NEW.lastAccessIndex > OLD.lastAccessIndex AND ", "NEW.") \
  PIN_TRIGGER("i", "INSERT", "", "NEW.") PIN_TRIGGER("d", "DELETE", "", "OLD.")

static int drop_pin_triggers(sqlite3* db, const char* user) {
  char sql[256];
  snprintf(sql, sizeof sql, "DROP TRIGGER IF EXISTS lineup_pin_u_%1$s; DROP TRIGGER IF EXISTS lineup_pin_i_%1$s;"
           "DROP TRIGGER IF EXISTS lineup_pin_d_%1$s;", user);
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

// Removes everything Lineup adds to app.db: every profile's triggers and the three tables.
static int remove_pins(sqlite3* db) {
  static char names[MAX_GAMES][64];
  int count = 0;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'trigger' AND name GLOB 'lineup_pin_[uid]_*'",
                         -1, &statement, NULL) != SQLITE_OK) return -1;
  while (count < MAX_GAMES && sqlite3_step(statement) == SQLITE_ROW)
    snprintf(names[count++], sizeof names[0], "%s", (const char*)sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
  for (int i = 0; i < count; ++i) {
    char sql[128];
    snprintf(sql, sizeof sql, "DROP TRIGGER \"%s\"", names[i]);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
  }
  return sqlite3_exec(db, "DROP TABLE IF EXISTS lineup_pin; DROP TABLE IF EXISTS lineup_seq; DROP TABLE IF EXISTS lineup_tmp;",
                      NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static int forget_pins(sqlite3* db, const char* user) {
  if (!has_object(db, "lineup_pin")) return 0;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "DELETE FROM lineup_pin WHERE user = ?", -1, &statement, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(statement, 1, user, -1, SQLITE_STATIC);
  int failed = sqlite3_step(statement) != SQLITE_DONE;
  sqlite3_finalize(statement);
  return failed ? -1 : 0;
}

// Once no profile has a pinned game, nothing of Lineup's stays in app.db.
static int remove_pins_if_unused(sqlite3* db) {
  if (has_object(db, "lineup_pin")) {
    sqlite3_stmt* statement;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM lineup_pin LIMIT 1", -1, &statement, NULL) != SQLITE_OK) return -1;
    int used = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    if (used) return 0;
  }
  return remove_pins(db);
}

// 1 when `user` has a pinned app on either tab, 0 when not, -1 on failure.
static int has_pins(sqlite3* db, const char* user) {
  if (!has_object(db, "lineup_pin")) return 0;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM lineup_pin WHERE user = ? LIMIT 1", -1, &statement, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(statement, 1, user, -1, SQLITE_STATIC);
  int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return step == SQLITE_ROW ? 1 : step == SQLITE_DONE ? 0 : -1;
}

// Inside save_order's transaction, after its triggers were dropped: records the posted apps of tab `location` that
// are pinned, at their posted places, and creates the triggers again while the profile has pins on either tab.
static int save_pins(sqlite3* db, const char* user, int location, const Row* posted, int count) {
  int pins = 0;
  for (int i = 0; i < count; ++i) pins += posted[i].pinned;
  static char sql[16 * 1024];
  // The other tab's pins stay; the rest are this tab's, or of apps that are gone.
  if (has_object(db, "lineup_pin")) {
    snprintf(sql, sizeof sql, "DELETE FROM lineup_pin WHERE user = '%1$s' AND titleId NOT IN (SELECT titleId FROM "
             "tbl_iconinfo_%1$s WHERE visible = 1 AND " TAB_OF("") " = %2$d)", user,
             location == GAMES_TAB ? MEDIA_TAB : GAMES_TAB);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
  }
  if (!pins) {
    int kept = has_pins(db, user);
    if (kept < 0) return -1;
    if (!kept) return remove_pins_if_unused(db);
    return snprintf(sql, sizeof sql, PIN_TRIGGERS, user) >= (int)sizeof sql ||
           sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK ? -1 : 0;
  }
  if (sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS lineup_pin (user TEXT, titleId TEXT, slot INTEGER, PRIMARY KEY (user, titleId));"
                   "CREATE TABLE IF NOT EXISTS lineup_seq (i INTEGER PRIMARY KEY);"
                   "CREATE TABLE IF NOT EXISTS lineup_tmp (titleId TEXT PRIMARY KEY, v INTEGER, r INTEGER, nv INTEGER);",
                   NULL, NULL, NULL) != SQLITE_OK) return -1;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO lineup_seq VALUES (?)", -1, &statement, NULL) != SQLITE_OK) return -1;
  int failed = 0;
  for (int i = 0; !failed && i < MAX_GAMES; ++i) {
    sqlite3_bind_int(statement, 1, i);
    failed = sqlite3_step(statement) != SQLITE_DONE;
    sqlite3_reset(statement);
  }
  sqlite3_finalize(statement);
  if (failed || sqlite3_prepare_v2(db, "INSERT INTO lineup_pin VALUES (?, ?, ?)", -1, &statement, NULL) != SQLITE_OK) return -1;
  for (int i = 0; !failed && i < count; ++i) {
    if (!posted[i].pinned) continue;
    sqlite3_bind_text(statement, 1, user, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 2, posted[i].title, -1, SQLITE_STATIC);
    sqlite3_bind_int(statement, 3, i);
    failed = sqlite3_step(statement) != SQLITE_DONE;
    sqlite3_reset(statement);
  }
  sqlite3_finalize(statement);
  return failed || snprintf(sql, sizeof sql, PIN_TRIGGERS, user) >= (int)sizeof sql ||
         sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK ? -1 : 0;
}

// Each of `rows`' pinned slot in `slots`, -1 when it is not pinned. Returns "on", "missing" (pinned games without
// their triggers, as after the database is rebuilt) or "off".
static const char* read_pins(sqlite3* db, const char* user, const Row* rows, int count, int* slots) {
  for (int i = 0; i < count; ++i) slots[i] = -1;
  sqlite3_stmt* statement;
  if (!has_object(db, "lineup_pin") ||
      sqlite3_prepare_v2(db, "SELECT titleId, slot FROM lineup_pin WHERE user = ?", -1, &statement, NULL) != SQLITE_OK)
    return "off";
  sqlite3_bind_text(statement, 1, user, -1, SQLITE_STATIC);
  int any = 0;
  for (; sqlite3_step(statement) == SQLITE_ROW; any = 1) {
    const char* title = (const char*)sqlite3_column_text(statement, 0);
    for (int i = 0; title && i < count; ++i)
      if (!strcmp(rows[i].title, title)) slots[i] = sqlite3_column_int(statement, 1);
  }
  sqlite3_finalize(statement);
  if (!any) return "off";
  for (const char* kind = "uid"; *kind; ++kind) {
    char name[64];
    snprintf(name, sizeof name, "lineup_pin_%c_%s", *kind, user);
    if (!has_object(db, name)) return "missing";
  }
  return "on";
}

// ---- Hidden apps ----
//
// visible = 0 takes an app off the home screen, on that profile only. lineup_hidden lists what Lineup hid, so
// showing can only bring back those and never the system's own hidden entries. It goes with its last row.
//
// The console puts visible back to 1 on a restart, so a profile with hidden apps has two triggers that hide them
// again, whether the system updates the row or writes it anew. They read lineup_hidden: a write to the profile's
// table fails if the table goes before them.

// The system's tiles have places of their own, off both tabs, and Lineup can hide those on a tab too: all but Game
// Library, which stays, and Welcome, which Lineup shows instead (see show_welcome). The PS Store, the Media Gallery
// and App Library hide whatever their tab.
#define STORE_TITLE "NPXS40047"
#define GALLERY_TITLE "NPXS40075"
#define GAME_LIBRARY_TITLE "NPXS40071"
#define APP_LIBRARY_TITLE "NPXS40139"
#define KEPT_SYSTEM "('" GAME_LIBRARY_TITLE "', 'NPXS40063')"
#define LISTED_SYSTEM "('" STORE_TITLE "', '" GALLERY_TITLE "', '" APP_LIBRARY_TITLE "')"
#define HIDE_TRIGGER(kind, event) \
  "CREATE TRIGGER IF NOT EXISTS lineup_hide_" kind "_%1$s AFTER " event " ON tbl_iconinfo_%1$s WHEN NEW.visible = 1 AND " \
  "NEW.titleId IN (SELECT titleId FROM lineup_hidden WHERE user = '%1$s') BEGIN UPDATE tbl_iconinfo_%1$s SET visible = 0 " \
  "WHERE titleId = NEW.titleId; END;"
#define HIDE_TRIGGERS HIDE_TRIGGER("u", "UPDATE OF visible") HIDE_TRIGGER("i", "INSERT")

static int create_hide_triggers(sqlite3* db, const char* user) {
  char sql[1024];
  return snprintf(sql, sizeof sql, HIDE_TRIGGERS, user) >= (int)sizeof sql ||
         sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK ? -1 : 0;
}

static int drop_hide_triggers(sqlite3* db, const char* user) {
  char sql[128];
  snprintf(sql, sizeof sql, "DROP TRIGGER IF EXISTS lineup_hide_u_%1$s; DROP TRIGGER IF EXISTS lineup_hide_i_%1$s;", user);
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

// The profiles with hidden apps, or -1.
static int hidden_users(sqlite3* db, char users[][16]) {
  int count = 0;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT DISTINCT user FROM lineup_hidden", -1, &statement, NULL) != SQLITE_OK) return -1;
  while (count < MAX_GAMES && sqlite3_step(statement) == SQLITE_ROW) {
    const char* user = (const char*)sqlite3_column_text(statement, 0);
    if (user && is_user_id(user)) snprintf(users[count++], sizeof users[0], "%s", user);
  }
  sqlite3_finalize(statement);
  return count;
}

// Whether `sql`, with ?1 the user when it has one, finds a row: 1, 0, or -1 on failure.
static int finds_row(sqlite3* db, const char* sql, const char* user) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
  if (sqlite3_bind_parameter_count(statement)) sqlite3_bind_text(statement, 1, user, -1, SQLITE_STATIC);
  int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return step == SQLITE_ROW ? 1 : step == SQLITE_DONE ? 0 : -1;
}

// Runs `sql` with ?1 the title and ?2 the user, when it has a ?2. Returns the rows changed, or -1.
static int run_title(sqlite3* db, const char* sql, const char* user, const char* title) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  if (sqlite3_bind_parameter_count(statement) > 1) sqlite3_bind_text(statement, 2, user, -1, SQLITE_STATIC);
  int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return step == SQLITE_DONE ? sqlite3_changes(db) : -1;
}

// Takes `title` out of `user`'s pins; the triggers go with the profile's last one.
static int forget_pin(sqlite3* db, const char* user, const char* title) {
  if (!has_object(db, "lineup_pin")) return 0;
  if (run_title(db, "DELETE FROM lineup_pin WHERE titleId = ?1 AND user = ?2", user, title) < 0) return -1;
  int kept = has_pins(db, user);
  if (kept) return kept < 0 ? -1 : 0;
  return drop_pin_triggers(db, user) || remove_pins_if_unused(db);
}

// Hides or shows `title` on `user`, after a backup. Returns the HTTP status; `message` explains a failure.
static int set_hidden(const char* user, const char* title, int hide, char* backup, size_t backup_size, char* message,
                      size_t message_size) {
  if (hide && !strcmp(title, TITLE_ID)) { snprintf(message, message_size, "Lineup cannot hide itself."); return 400; }
  if (!strcmp(title, GAME_LIBRARY_TITLE) || !strcmp(title, "NPXS40063")) {
    snprintf(message, message_size, "That tile is not hidden by Lineup.");
    return 400;
  }
  sqlite3* db = open_db(1);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return 500; }
  int status = 500;
  if (!table_exists(db, user)) { snprintf(message, message_size, "No such profile."); status = 404; goto done; }
  if (!hide && !has_object(db, "lineup_hidden")) { snprintf(message, message_size, "Lineup did not hide that app."); status = 404; goto done; }
  if (check_and_backup(db, backup, backup_size, message, message_size)) goto done;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The database is busy (%s). Try again.", sqlite3_errmsg(db));
    goto done;
  }
  {
    char sql[768];
    int changed, failed = 0;
    if (hide) {
      // Another system tile only from a tab: one off both never shows on the home screen.
      snprintf(sql, sizeof sql, "UPDATE tbl_iconinfo_%s SET visible = 0 WHERE titleId = ?1 AND visible = 1 AND "
               "(dispLocation IN (%d, %d) OR titleId IN " LISTED_SYSTEM " OR (titleId GLOB 'NPXS*' AND titleId NOT IN "
               KEPT_SYSTEM " AND (%s & 6) <> 0))", user, GAMES_TAB, MEDIA_TAB,
               has_column(db, "main", "tbl_contentinfo", "dispLocation")
               ? "ifnull((SELECT c.dispLocation FROM tbl_contentinfo c WHERE c.titleId = ?1), dispLocation)"
               : "dispLocation");
      changed = run_title(db, sql, user, title);
      failed = changed < 0 || (changed && (sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS lineup_hidden (user TEXT, titleId "
               "TEXT, PRIMARY KEY (user, titleId))", NULL, NULL, NULL) != SQLITE_OK ||
               run_title(db, "INSERT OR IGNORE INTO lineup_hidden VALUES (?2, ?1)", user, title) < 0 ||
               create_hide_triggers(db, user) || forget_pin(db, user, title)));
    } else {
      // The row goes first: while it is there, the triggers hide the app again.
      changed = run_title(db, "DELETE FROM lineup_hidden WHERE titleId = ?1 AND user = ?2", user, title);
      snprintf(sql, sizeof sql, "UPDATE tbl_iconinfo_%s SET visible = 1 WHERE titleId = ?1 AND visible = 0", user);
      failed = changed < 0 || (changed && run_title(db, sql, user, title) < 0);
      int others = failed ? 0 : finds_row(db, "SELECT 1 FROM lineup_hidden WHERE user = ?1 LIMIT 1", user);
      int any = failed ? 0 : finds_row(db, "SELECT 1 FROM lineup_hidden LIMIT 1", user);
      failed = failed || others < 0 || any < 0 || (!others && drop_hide_triggers(db, user)) ||
               (!any && sqlite3_exec(db, "DROP TABLE lineup_hidden", NULL, NULL, NULL) != SQLITE_OK);
    }
    if (failed) snprintf(message, message_size, "The write failed (%s). Nothing changed.", sqlite3_errmsg(db));
    else if (!changed) { snprintf(message, message_size, "That app changed since it was read. Open it again."); status = 409; failed = 1; }
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      if (!failed) snprintf(message, message_size, "The write failed (%s). Nothing changed.", sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      goto done;
    }
  }
  status = 200;
done:
  sqlite3_close(db);
  return status;
}

// Inside a transaction: brings back everything Lineup hid, on every profile, and drops lineup_hidden.
static int show_all(sqlite3* db) {
  if (!has_object(db, "lineup_hidden")) return 0;
  static char users[MAX_GAMES][16];
  int count = hidden_users(db, users);
  if (count < 0) return -1;
  for (int i = 0; i < count; ++i) {
    char sql[256];
    snprintf(sql, sizeof sql, "UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE visible = 0 AND titleId IN (SELECT titleId "
             "FROM lineup_hidden WHERE user = '%1$s')", users[i]);
    if (drop_hide_triggers(db, users[i]) ||
        (table_exists(db, users[i]) && sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK)) return -1;
  }
  return sqlite3_exec(db, "DROP TABLE lineup_hidden", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

// Gives the profiles that hid apps on 1.6.0 their triggers, and hides again whatever came back, after a backup.
// Returns how many profiles it changed, or -1.
static int restore_hidden(void) {
  sqlite3* db = open_db(1);
  if (!db) return -1;
  static char users[MAX_GAMES][16], stale[MAX_GAMES][16];
  int count = has_object(db, "lineup_hidden") ? hidden_users(db, users) : 0, stale_count = 0, failed = count < 0;
  for (int i = 0; !failed && i < count; ++i) {
    char sql[256], name[64];
    if (!table_exists(db, users[i])) continue;
    snprintf(sql, sizeof sql, "SELECT 1 FROM tbl_iconinfo_%1$s WHERE visible = 1 AND titleId IN (SELECT titleId FROM "
             "lineup_hidden WHERE user = '%1$s') LIMIT 1", users[i]);
    int back = finds_row(db, sql, users[i]);
    snprintf(name, sizeof name, "lineup_hide_u_%s", users[i]);
    int missing = !has_object(db, name);
    snprintf(name, sizeof name, "lineup_hide_i_%s", users[i]);
    missing = missing || !has_object(db, name);
    failed = back < 0;
    if (back > 0 || missing) snprintf(stale[stale_count++], sizeof stale[0], "%s", users[i]);
  }
  if (!failed && stale_count) {
    char backup[256] = "", message[300] = "";
    failed = check_and_backup(db, backup, sizeof backup, message, sizeof message) ||
             sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK;
    for (int i = 0; !failed && i < stale_count; ++i) {
      char sql[256];
      snprintf(sql, sizeof sql, "UPDATE tbl_iconinfo_%1$s SET visible = 0 WHERE visible = 1 AND titleId IN (SELECT titleId "
               "FROM lineup_hidden WHERE user = '%1$s')", stale[i]);
      failed = sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK || create_hide_triggers(db, stale[i]);
    }
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      LOG("hidden apps not restored: %s", *message ? message : sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      failed = 1;
    } else LOG("hidden apps restored for %d profiles, backup %s", stale_count, backup);
  }
  sqlite3_close(db);
  return failed ? -1 : stale_count;
}

// Replaces the triggers of 1.4.3 and older, which kept pins on the Games tab only, after a backup. Returns how many
// profiles it updated, or -1.
static int update_pin_triggers(void) {
  sqlite3* db = open_db(1);
  if (!db) return -1;
  static char users[MAX_GAMES][16];
  int count = 0, failed = 0, step = SQLITE_DONE;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT DISTINCT substr(name, 14) FROM sqlite_master WHERE type = 'trigger' AND name GLOB "
                         "'lineup_pin_[uid]_*' AND sql NOT LIKE '%" PIN_TABS "%'", -1, &statement, NULL) != SQLITE_OK)
    failed = 1;
  else {
    while (count < MAX_GAMES && (step = sqlite3_step(statement)) == SQLITE_ROW) {
      const char* user = (const char*)sqlite3_column_text(statement, 0);
      if (user && is_user_id(user)) snprintf(users[count++], sizeof users[0], "%s", user);
    }
    // A scan cut short by an error would leave the other profiles with the old triggers.
    if (step != SQLITE_ROW && step != SQLITE_DONE) failed = 1;
    sqlite3_finalize(statement);
  }
  if (failed) LOG("pin triggers not updated: %s", sqlite3_errmsg(db));
  if (!failed && count) {
    char backup[256] = "", message[300] = "";
    static char sql[16 * 1024];
    failed = check_and_backup(db, backup, sizeof backup, message, sizeof message) ||
             sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK;
    for (int i = 0; !failed && i < count; ++i)
      failed = drop_pin_triggers(db, users[i]) || snprintf(sql, sizeof sql, PIN_TRIGGERS, users[i]) >= (int)sizeof sql ||
               sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK;
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      LOG("pin triggers not updated: %s", *message ? message : sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      failed = 1;
    } else LOG("pin triggers updated for %d profiles, backup %s", count, backup);
  }
  sqlite3_close(db);
  return failed ? -1 : count;
}

// Saves `posted` as the new order of tab `location`. Returns the HTTP status; `message` explains a failure.
static int save_order(const char* user, int location, const Row* posted, int posted_count, char* backup,
                      size_t backup_size, char* integrity, size_t integrity_size, char* message, size_t message_size) {
  // With every home place pinned, the last app opened (the jailbreak's entry, say) gets no place there and ends up
  // in the library: one stays free.
  int limit = location == MEDIA_TAB ? MEDIA_HOME_LIMIT : HOME_LIMIT;
  int home = posted_count < limit ? posted_count : limit, free_places = 0;
  for (int i = 0; i < home; ++i) free_places += !posted[i].pinned;
  if (home && !free_places) {
    snprintf(message, message_size, "Leave one of the first %d places unpinned. Nothing was saved.", home);
    return 400;
  }
  sqlite3* db = open_db(1);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return 500; }
  int status = 500;
  static Row current[MAX_GAMES];
  static long long indices[MAX_GAMES];
  if (!table_exists(db, user)) { snprintf(message, message_size, "No such profile."); status = 404; goto done; }
  if (check_and_backup(db, backup, backup_size, message, message_size)) goto done;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The database is busy (%s). Try again.", sqlite3_errmsg(db));
    goto done;
  }
  int count = read_tab(db, user, location, current, NULL, MAX_GAMES);
  if (count < 0 || plan_reorder(current, count, posted, posted_count, indices)) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    snprintf(message, message_size, "The row changed since it was read. Open it again.");
    status = 409;
    goto done;
  }
  {
    char sql[128];
    snprintf(sql, sizeof sql, "UPDATE tbl_iconinfo_%s SET lastAccessIndex = ? WHERE titleId = ?", user);
    sqlite3_stmt* update = NULL;
    // The pin triggers go first, or these writes would fire them.
    int failed = drop_pin_triggers(db, user) || sqlite3_prepare_v2(db, sql, -1, &update, NULL) != SQLITE_OK;
    for (int i = 0; !failed && i < posted_count; ++i) {
      sqlite3_bind_int64(update, 1, indices[i]);
      sqlite3_bind_text(update, 2, posted[i].title, -1, SQLITE_STATIC);
      failed = sqlite3_step(update) != SQLITE_DONE || sqlite3_changes(db) != 1;
      sqlite3_reset(update);
    }
    if (!failed) failed = save_pins(db, user, location, posted, posted_count);
    if (failed) snprintf(message, message_size, "The write failed (%s). Nothing changed.", sqlite3_errmsg(db));
    sqlite3_finalize(update);
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      if (!failed) snprintf(message, message_size, "The write failed (%s). Nothing changed.", sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      goto done;
    }
  }
  pragma_text(db, "PRAGMA integrity_check", integrity, integrity_size);
  if (strcmp(integrity, "ok")) LOG("integrity_check after save: %s (backup %s)", integrity, backup);
  status = 200;
done:
  sqlite3_close(db);
  return status;
}

// ---- Moving apps between tabs ----
//
// What puts an app on a tab is written in several places, and all of them must agree, or it ends up half game, half
// media app: dispLocation and hubAppUri on every profile's tbl_iconinfo_<user>; dispLocation, categoryType,
// is3rdPartyVideoApp, pprHubAppUri and the same values in AppInfoJson on tbl_contentinfo; and DISPLAYLOCATION,
// CATEGORY_TYPE and HUBAPP_URI on appinfo.db's tbl_appinfo. They get what a native app of the tab holds. Bit 65536 of
// the category is the Media one, so the rest of it survives a move there and back. Both databases commit together.
//
// Installing an app again writes it back on the tab its param.json names, and some payloads (Game Compressor) do
// that each time they start. So lineup_moved, in both databases, keeps each moved app's tab, and triggers put the app
// back there whenever the system writes it anew or changes its tab. They leave AppInfoJson alone, since the system's
// SQLite may lack the JSON functions; restore_moved fixes it when Lineup starts. An app moved back to the tab it
// started on (`home`) leaves lineup_moved, and the last one takes the tables and triggers with it.
#define MEDIA_CATEGORY 65536
#define MOVED_TAB "(SELECT tab FROM lineup_moved WHERE titleId = NEW.titleId)"
#define HUB_OF(tab) "(CASE " tab " WHEN 188 THEN 'psmediahub:main?titleId=' ELSE 'pshome:gamehub?titleId=' END || NEW.titleId)"
// Only a row on a tab and off the one it was moved to: the triggers' own writes do not fire them again.
#define MOVE_WHEN "WHEN NEW.dispLocation IN (138, 188) AND EXISTS (SELECT 1 FROM lineup_moved WHERE titleId = " \
  "NEW.titleId AND tab <> NEW.dispLocation) BEGIN "
#define CONTENT_TRIGGER(kind, event) \
  "CREATE TRIGGER IF NOT EXISTS lineup_move_c" kind " AFTER " event " ON tbl_contentinfo " MOVE_WHEN \
  "UPDATE tbl_contentinfo SET dispLocation = " MOVED_TAB ", categoryType = CASE " MOVED_TAB " WHEN 188 THEN " \
  "categoryType | 65536 ELSE categoryType & ~65536 END, is3rdPartyVideoApp = " MOVED_TAB " = 188, pprHubAppUri = " \
  HUB_OF(MOVED_TAB) " WHERE titleId = NEW.titleId; END;"
#define ICON_TRIGGER(kind, event) \
  "CREATE TRIGGER IF NOT EXISTS lineup_move_" kind "_%1$s AFTER " event " ON tbl_iconinfo_%1$s " MOVE_WHEN \
  "UPDATE tbl_iconinfo_%1$s SET dispLocation = " MOVED_TAB ", hubAppUri = " HUB_OF(MOVED_TAB) " WHERE titleId = " \
  "NEW.titleId; END;"
// tbl_appinfo holds one row per key; a row that already has its value fires nothing.
#define INFO_VALUE "CASE NEW.key WHEN 'DISPLAYLOCATION' THEN " MOVED_TAB " WHEN 'CATEGORY_TYPE' THEN CASE " MOVED_TAB \
  " WHEN 188 THEN NEW.val | 65536 ELSE NEW.val & ~65536 END ELSE " HUB_OF(MOVED_TAB) " END"
#define INFO_TRIGGER(kind, event) \
  "CREATE TRIGGER IF NOT EXISTS info.lineup_move_a" kind " AFTER " event " ON tbl_appinfo WHEN NEW.key IN " \
  "('DISPLAYLOCATION', 'CATEGORY_TYPE', 'HUBAPP_URI') AND EXISTS (SELECT 1 FROM lineup_moved WHERE titleId = " \
  "NEW.titleId) AND NEW.val IS NOT (" INFO_VALUE ") BEGIN UPDATE tbl_appinfo SET val = " INFO_VALUE " WHERE " \
  "titleId = NEW.titleId AND key = NEW.key; END;"

// The concept places, for an app whose concept is its own (a concept another title shares is left alone):
// dispLocation and hubAppUri on every profile's tbl_concepticoninfo_<user>, where a tab is kept 1024 (Games) or 2048
// (Media) above its dispLocation, 1162 and 2236; dispLocation, pprHubAppUri and display_location in ConceptInfoJson
// on tbl_conceptmetadata; and display_location and hubapp_uri on appinfo.db's tbl_conceptinfo. Lineup up to 1.8.0
// left them on the old tab. lineup_moved_concept, in both databases, gives their triggers (lineup_move_k*) the
// concept's title and tab. A place, and its triggers, are written only when the firmware has their columns.
#define CONCEPT_OF(tab) "(" tab " + CASE " tab " WHEN 188 THEN 2048 ELSE 1024 END)"
#define K_TAB(key) "(SELECT tab FROM lineup_moved_concept WHERE concept = NEW." key ")"
#define K_HUB(key) "(CASE " K_TAB(key) " WHEN 188 THEN 'psmediahub:main?titleId=' ELSE 'pshome:gamehub?titleId=' END || " \
  "(SELECT titleId FROM lineup_moved_concept WHERE concept = NEW." key "))"
#define K_ICON_TRIGGER(kind, event) \
  "CREATE TRIGGER IF NOT EXISTS lineup_move_k" kind "_%1$s AFTER " event " ON tbl_concepticoninfo_%1$s WHEN " \
  "NEW.dispLocation IN (1162, 2236) AND EXISTS (SELECT 1 FROM lineup_moved_concept WHERE concept = NEW.localConceptId " \
  "AND " CONCEPT_OF("tab") " <> NEW.dispLocation) BEGIN UPDATE tbl_concepticoninfo_%1$s SET dispLocation = " \
  CONCEPT_OF(K_TAB("localConceptId")) ", hubAppUri = " K_HUB("localConceptId") " WHERE localConceptId = " \
  "NEW.localConceptId; END;"
#define K_META_TRIGGER(kind, event) \
  "CREATE TRIGGER IF NOT EXISTS lineup_move_km" kind " AFTER " event " ON tbl_conceptmetadata WHEN NEW.dispLocation IN " \
  "(138, 188) AND EXISTS (SELECT 1 FROM lineup_moved_concept WHERE concept = NEW.localConceptId AND tab <> " \
  "NEW.dispLocation) BEGIN UPDATE tbl_conceptmetadata SET dispLocation = " K_TAB("localConceptId") ", pprHubAppUri = " \
  K_HUB("localConceptId") " WHERE localConceptId = NEW.localConceptId; END;"
#define K_INFO_VALUE "CASE NEW.key WHEN 'display_location' THEN " K_TAB("local_concept_id") " ELSE " \
  K_HUB("local_concept_id") " END"
#define K_INFO_TRIGGER(kind, event) \
  "CREATE TRIGGER IF NOT EXISTS info.lineup_move_ka" kind " AFTER " event " ON tbl_conceptinfo WHEN NEW.key IN " \
  "('display_location', 'hubapp_uri') AND EXISTS (SELECT 1 FROM lineup_moved_concept WHERE concept = " \
  "NEW.local_concept_id) AND NEW.val IS NOT (" K_INFO_VALUE ") BEGIN UPDATE tbl_conceptinfo SET val = " K_INFO_VALUE \
  " WHERE local_concept_id = NEW.local_concept_id AND key = NEW.key; END;"

// `json` when it is valid, else an empty object: json_each fails the whole statement on malformed JSON.
#define SAFE_JSON(json) "CASE WHEN json_valid(" json ") THEN " json " ELSE '{}' END"

// AppInfoJson comes in two shapes: flat ({"DISPLAYLOCATION":138, ...}), as the console writes it on 11.00 and later,
// and a field list ({"field_list":[{"data":138,"key":"DISPLAYLOCATION", ...}, ...]}), as older firmware and some
// package installers write it. APP_FIELD reads `key` from either, the field list first.
#define APP_FIELD(json, key) "coalesce((SELECT e.value ->> '$.data' FROM json_each(" SAFE_JSON(json) ", '$.field_list') e " \
  "WHERE e.value ->> '$.key' = '" key "' LIMIT 1), " SAFE_JSON(json) " ->> '$." key "')"

// Sets `key` of `title`'s AppInfoJson to `text`, or to `number` when `text` is NULL: in a field list, the data of
// its entry (and the size, for text), and a copy of the key an earlier Lineup put at the top goes; else the key at
// the top. Returns 0, or -1 on failure.
static int set_app_field(sqlite3* db, const char* title, const char* key, long long number, const char* text) {
  char sql[1024];
  snprintf(sql, sizeof sql,
           "UPDATE tbl_contentinfo SET AppInfoJson = CASE WHEN json_type(AppInfoJson, '$.field_list') = 'array' THEN "
           "json_remove(ifnull((SELECT CASE WHEN typeof(?2) = 'text' AND json_type(e.value, '$.size') IS NOT NULL THEN "
           "json_set(AppInfoJson, '$.field_list[' || e.key || '].data', ?2, '$.field_list[' || e.key || '].size', "
           "length(CAST(?2 AS BLOB))) ELSE json_set(AppInfoJson, '$.field_list[' || e.key || '].data', ?2) END FROM "
           "json_each(AppInfoJson, '$.field_list') e WHERE e.value ->> '$.key' = '%1$s' LIMIT 1), AppInfoJson), '$.%1$s') "
           "ELSE json_set(AppInfoJson, '$.%1$s', ?2) END WHERE titleId = ?1 AND json_valid(AppInfoJson)", key);
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  if (text) sqlite3_bind_text(statement, 2, text, -1, SQLITE_STATIC);
  else sqlite3_bind_int64(statement, 2, number);
  int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return step == SQLITE_DONE ? 0 : -1;
}

// Whether `schema`.`table` has every column of `columns` (NULL-terminated).
static int has_columns(sqlite3* db, const char* schema, const char* table, const char* const* columns) {
  for (; *columns; ++columns) if (!has_column(db, schema, table, *columns)) return 0;
  return 1;
}

static const char* const CONCEPT_ICON_COLUMNS[] = {"localConceptId", "dispLocation", "hubAppUri", NULL};
static const char* const CONCEPT_META_COLUMNS[] = {"localConceptId", "dispLocation", "pprHubAppUri", NULL};
static const char* const CONCEPT_INFO_COLUMNS[] = {"local_concept_id", "key", "val", NULL};

// Runs `sql` with ?1 the number and ?2, ?3 the texts, as far as it has them. Returns the rows changed, or -1.
static int run_values(sqlite3* db, const char* sql, long long number, const char* second, const char* third) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
  int params = sqlite3_bind_parameter_count(statement);
  if (params >= 1) sqlite3_bind_int64(statement, 1, number);
  if (params >= 2) sqlite3_bind_text(statement, 2, second, -1, SQLITE_STATIC);
  if (params >= 3) sqlite3_bind_text(statement, 3, third, -1, SQLITE_STATIC);
  int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return step == SQLITE_DONE ? sqlite3_changes(db) : -1;
}

// `title`'s concept into `out` when no other title has it: 1, 0 when it has none or shares it, -1 on failure.
static int own_concept(sqlite3* db, const char* title, char* out, size_t size) {
  if (!has_column(db, "main", "tbl_contentinfo", "localConceptId")) return 0;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT localConceptId FROM tbl_contentinfo c WHERE titleId = ?1 AND ifnull(localConceptId, "
                         "'') <> '' AND NOT EXISTS (SELECT 1 FROM tbl_contentinfo o WHERE o.localConceptId = "
                         "c.localConceptId AND o.titleId <> ?1)", -1, &statement, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  int step = sqlite3_step(statement), found = step == SQLITE_ROW;
  if (found) snprintf(out, size, "%s", (const char*)sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
  return found ? 1 : step == SQLITE_DONE ? 0 : -1;
}

// ConceptInfoJson keeps the concept's tab as the data of its display_location field: made to match dispLocation.
// Only that value changes; a concept without the field is left as it is.
static int fix_concept_json(sqlite3* db, const char* concept) {
  if (!has_column(db, "main", "tbl_conceptmetadata", "ConceptInfoJson")) return 0;
  return run_values(db, "UPDATE tbl_conceptmetadata SET ConceptInfoJson = json_set(ConceptInfoJson, '$.field_list[' || "
                    "(SELECT e.key FROM json_each(" SAFE_JSON("ConceptInfoJson") ", '$.field_list') e WHERE e.value ->> '$.key' = "
                    "'display_location' LIMIT 1) || '].data', dispLocation) WHERE localConceptId = ?2 AND "
                    "json_valid(ConceptInfoJson) AND EXISTS (SELECT 1 FROM json_each(" SAFE_JSON("ConceptInfoJson") ", '$.field_list') e "
                    "WHERE e.value ->> '$.key' = 'display_location' AND e.value ->> '$.data' IS NOT dispLocation)",
                    0, concept, NULL) < 0 ? -1 : 0;
}

// The concept triggers that are missing (create = 0), or creates them (create = 1: inside a transaction, with
// appinfo.db attached as info and lineup_moved_concept in both). Returns how many were missing, or -1.
static int concept_triggers(sqlite3* db, char users[][16], int count, int create) {
  int missing = 0;
  static char sql[4096];
  if (has_columns(db, "main", "tbl_conceptmetadata", CONCEPT_META_COLUMNS) &&
      (!has_object_in(db, "main", "lineup_move_kmi") || !has_object_in(db, "main", "lineup_move_kmu"))) {
    ++missing;
    if (create && sqlite3_exec(db, K_META_TRIGGER("i", "INSERT") K_META_TRIGGER("u", "UPDATE OF dispLocation"), NULL,
                               NULL, NULL) != SQLITE_OK) return -1;
  }
  if (has_columns(db, "info", "tbl_conceptinfo", CONCEPT_INFO_COLUMNS) &&
      (!has_object_in(db, "info", "lineup_move_kai") || !has_object_in(db, "info", "lineup_move_kau"))) {
    ++missing;
    if (create && sqlite3_exec(db, K_INFO_TRIGGER("i", "INSERT") K_INFO_TRIGGER("u", "UPDATE OF val"), NULL, NULL,
                               NULL) != SQLITE_OK) return -1;
  }
  for (int i = 0; i < count; ++i) {
    char table[64], insert[64], update[64];
    snprintf(table, sizeof table, "tbl_concepticoninfo_%s", users[i]);
    snprintf(insert, sizeof insert, "lineup_move_ki_%s", users[i]);
    snprintf(update, sizeof update, "lineup_move_ku_%s", users[i]);
    if (!has_columns(db, "main", table, CONCEPT_ICON_COLUMNS) ||
        (has_object_in(db, "main", insert) && has_object_in(db, "main", update))) continue;
    ++missing;
    if (create && (snprintf(sql, sizeof sql, K_ICON_TRIGGER("i", "INSERT") K_ICON_TRIGGER("u", "UPDATE OF dispLocation"),
                            users[i]) >= (int)sizeof sql || sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK))
      return -1;
  }
  return missing;
}

// Inside a transaction, with appinfo.db attached as info: keeps `title`'s concept and tab for the concept triggers,
// when the concept is its own.
static int record_concept(sqlite3* db, const char* title, int location) {
  char concept[96];
  int own = own_concept(db, title, concept, sizeof concept);
  if (own <= 0) return own;
  const char* const sql[] = {"INSERT OR REPLACE INTO main.lineup_moved_concept VALUES (?2, ?3, ?1)",
                             "INSERT OR REPLACE INTO info.lineup_moved_concept VALUES (?2, ?3, ?1)"};
  for (int i = 0; i < 2; ++i) if (run_values(db, sql[i], location, concept, title) < 0) return -1;
  return 0;
}

// Inside a transaction, with appinfo.db attached as info: puts `title`'s own concept on tab `location` in every place
// the firmware has.
static int write_concepts(sqlite3* db, const char* title, int location, char users[][16], int count) {
  char concept[96], hub[96];
  int own = own_concept(db, title, concept, sizeof concept);
  if (own <= 0) return own;
  int media = location == MEDIA_TAB;
  snprintf(hub, sizeof hub, media ? "psmediahub:main?titleId=%s" : "pshome:gamehub?titleId=%s", title);
  for (int i = 0; i < count; ++i) {
    char table[64], sql[200];
    snprintf(table, sizeof table, "tbl_concepticoninfo_%s", users[i]);
    if (!has_columns(db, "main", table, CONCEPT_ICON_COLUMNS)) continue;
    snprintf(sql, sizeof sql, "UPDATE %s SET dispLocation = ?1, hubAppUri = ?2 WHERE localConceptId = ?3", table);
    if (run_values(db, sql, location + (media ? 2048 : 1024), hub, concept) < 0) return -1;
  }
  if (has_columns(db, "main", "tbl_conceptmetadata", CONCEPT_META_COLUMNS) &&
      (run_values(db, "UPDATE tbl_conceptmetadata SET dispLocation = ?1, pprHubAppUri = ?2 WHERE localConceptId = ?3",
                  location, hub, concept) < 0 || fix_concept_json(db, concept))) return -1;
  if (has_columns(db, "info", "tbl_conceptinfo", CONCEPT_INFO_COLUMNS) &&
      run_values(db, "UPDATE info.tbl_conceptinfo SET val = CASE key WHEN 'display_location' THEN ?1 ELSE ?2 END WHERE "
                 "local_concept_id = ?3 AND key IN ('display_location', 'hubapp_uri')", location, hub, concept) < 0)
    return -1;
  return 0;
}

// The profiles, from their tbl_iconinfo_<user> tables, or -1.
static int icon_users(sqlite3* db, char users[][16]) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT substr(name, 14) FROM main.sqlite_master WHERE type = 'table' AND name GLOB "
                         "'tbl_iconinfo_[0-9]*'", -1, &statement, NULL) != SQLITE_OK) return -1;
  int count = 0, step = SQLITE_DONE;
  while (count < MAX_GAMES && (step = sqlite3_step(statement)) == SQLITE_ROW) {
    const char* user = (const char*)sqlite3_column_text(statement, 0);
    if (user && is_user_id(user)) snprintf(users[count++], sizeof users[0], "%s", user);
  }
  sqlite3_finalize(statement);
  return step == SQLITE_ROW || step == SQLITE_DONE ? count : -1;
}

// Attaches appinfo.db as info and checks it. Returns 0, or -1 with `message` set.
static int attach_appinfo(sqlite3* db, char* message, size_t message_size) {
  char sql[400], check[128];
  snprintf(sql, sizeof sql, "ATTACH '%s' AS info", appinfo_path);
  if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "Could not open appinfo.db (%s). Nothing changed.", sqlite3_errmsg(db));
    return -1;
  }
  if (pragma_text(db, "PRAGMA info.quick_check", check, sizeof check)) {
    snprintf(message, message_size, "appinfo.db failed its check (%s). Nothing changed.", check);
    return -1;
  }
  return 0;
}

// Copies the attached appinfo.db beside `backup`, app.db's copy. Returns 0, or -1 with `message` set.
static int backup_appinfo(sqlite3* db, const char* backup, char* message, size_t message_size) {
  char info_backup[300], sql[400];
  snprintf(info_backup, sizeof info_backup, "%.*s.appinfo.db", (int)strlen(backup) - 3, backup);
  unlink(info_backup);
  snprintf(sql, sizeof sql, "VACUUM info INTO '%s'", info_backup);
  if (sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK) return 0;
  snprintf(message, message_size, "The backup failed (%s). Nothing changed.", sqlite3_errmsg(db));
  return -1;
}

#define MOVED_CONCEPT_TABLES \
  "CREATE TABLE IF NOT EXISTS main.lineup_moved_concept (concept TEXT PRIMARY KEY, titleId TEXT, tab INTEGER);" \
  "CREATE TABLE IF NOT EXISTS info.lineup_moved_concept (concept TEXT PRIMARY KEY, titleId TEXT, tab INTEGER);"

// Inside a transaction, with appinfo.db attached as info: the triggers of every place, and the concept tables their
// triggers read, made before them.
static int ensure_moves(sqlite3* db, char users[][16], int count) {
  if (sqlite3_exec(db, CONTENT_TRIGGER("i", "INSERT") CONTENT_TRIGGER("u", "UPDATE OF dispLocation")
                   INFO_TRIGGER("i", "INSERT") INFO_TRIGGER("u", "UPDATE OF val") MOVED_CONCEPT_TABLES, NULL, NULL,
                   NULL) != SQLITE_OK) return -1;
  static char sql[4096];
  for (int i = 0; i < count; ++i)
    if (snprintf(sql, sizeof sql, ICON_TRIGGER("i", "INSERT") ICON_TRIGGER("u", "UPDATE OF dispLocation"), users[i]) >=
        (int)sizeof sql || sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
  return concept_triggers(db, users, count, 1) < 0 ? -1 : 0;
}

// Inside a transaction, with appinfo.db attached as info: removes the triggers and tables of the moves, in both. The
// system tiles' (lineup_move_sys_*) have their own.
static int drop_moves(sqlite3* db) {
  static char names[4 * MAX_GAMES + 16][96];
  int count = 0;
  for (int schema = 0; schema < 2; ++schema) {
    char sql[200];
    snprintf(sql, sizeof sql, "SELECT name FROM %s.sqlite_master WHERE type = 'trigger' AND name GLOB 'lineup_move_*' "
             "AND name NOT GLOB 'lineup_move_sys_*'", schema ? "info" : "main");
    sqlite3_stmt* statement;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
    while (count < (int)(sizeof names / sizeof names[0]) && sqlite3_step(statement) == SQLITE_ROW)
      snprintf(names[count++], sizeof names[0], "%s.\"%s\"", schema ? "info" : "main",
               (const char*)sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
  }
  for (int i = 0; i < count; ++i) {
    char sql[128];
    snprintf(sql, sizeof sql, "DROP TRIGGER %s", names[i]);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
  }
  return sqlite3_exec(db, "DROP TABLE IF EXISTS main.lineup_moved; DROP TABLE IF EXISTS info.lineup_moved;"
                      "DROP TABLE IF EXISTS main.lineup_moved_concept; DROP TABLE IF EXISTS info.lineup_moved_concept;",
                      NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

// Inside move_app's transaction, before the write: keeps `title`'s new tab in lineup_moved, or lets the app go when
// it is back on the tab it started on.
static int record_move(sqlite3* db, const char* title, int from, int location, char users[][16], int count) {
  if (sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS main.lineup_moved (titleId TEXT PRIMARY KEY, tab INTEGER, home INTEGER);"
                   "CREATE TABLE IF NOT EXISTS info.lineup_moved (titleId TEXT PRIMARY KEY, tab INTEGER);", NULL, NULL,
                   NULL) != SQLITE_OK) return -1;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT home FROM main.lineup_moved WHERE titleId = ?", -1, &statement, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  int recorded = sqlite3_step(statement) == SQLITE_ROW, home = recorded ? sqlite3_column_int(statement, 0) : from;
  sqlite3_finalize(statement);
  // An app another tool moved is on the tab it was moved to, not its own. The Media bit of the category is the
  // install's, unless Lineup 1.6 or later changed it: the better guess at where an install puts it back.
  if (!recorded) {
    if (sqlite3_prepare_v2(db, "SELECT categoryType FROM tbl_contentinfo WHERE titleId = ?", -1, &statement, NULL) !=
        SQLITE_OK) return -1;
    sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
    if (sqlite3_step(statement) == SQLITE_ROW)
      home = sqlite3_column_int64(statement, 0) & MEDIA_CATEGORY ? MEDIA_TAB : GAMES_TAB;
    sqlite3_finalize(statement);
  }
  char sql[128];
  if (home == location) {
    if (run_title(db, "DELETE FROM main.lineup_moved WHERE titleId = ?1", NULL, title) < 0 ||
        run_title(db, "DELETE FROM info.lineup_moved WHERE titleId = ?1", NULL, title) < 0) return -1;
    if (has_object_in(db, "main", "lineup_moved_concept") &&
        (run_title(db, "DELETE FROM main.lineup_moved_concept WHERE titleId = ?1", NULL, title) < 0 ||
         run_title(db, "DELETE FROM info.lineup_moved_concept WHERE titleId = ?1", NULL, title) < 0)) return -1;
    int left = finds_row(db, "SELECT 1 FROM main.lineup_moved LIMIT 1", NULL);
    return left < 0 ? -1 : left ? 0 : drop_moves(db);
  }
  snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO main.lineup_moved VALUES (?1, %d, %d)", location, home);
  if (run_title(db, sql, NULL, title) < 0) return -1;
  snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO info.lineup_moved VALUES (?1, %d)", location);
  if (run_title(db, sql, NULL, title) < 0) return -1;
  return ensure_moves(db, users, count) || record_concept(db, title, location);
}

// Inside a transaction, with appinfo.db attached as info: puts `title` on tab `location` in every place.
static int write_move(sqlite3* db, const char* title, int location, char users[][16], int count) {
  int media = location == MEDIA_TAB;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT categoryType FROM tbl_contentinfo WHERE titleId = ?", -1, &statement, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  long long category = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : 0;
  sqlite3_finalize(statement);
  category = media ? category | MEDIA_CATEGORY : category & ~(long long)MEDIA_CATEGORY;
  char hub[96];
  snprintf(hub, sizeof hub, media ? "psmediahub:main?titleId=%s" : "pshome:gamehub?titleId=%s", title);
  const char* const shared[] = {
    "UPDATE tbl_contentinfo SET dispLocation = ?2, categoryType = ?3, is3rdPartyVideoApp = ?4, pprHubAppUri = ?5 "
    "WHERE titleId = ?1",
    "UPDATE info.tbl_appinfo SET val = CASE key WHEN 'DISPLAYLOCATION' THEN ?2 WHEN 'CATEGORY_TYPE' THEN ?3 ELSE ?5 END "
    "WHERE titleId = ?1 AND key IN ('DISPLAYLOCATION', 'CATEGORY_TYPE', 'HUBAPP_URI')",
  };
  const int expected[] = {1, 3};
  for (int i = 0; i < 2; ++i) {
    if (sqlite3_prepare_v2(db, shared[i], -1, &statement, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
    sqlite3_bind_int(statement, 2, location);
    sqlite3_bind_int64(statement, 3, category);
    sqlite3_bind_int(statement, 4, media);
    sqlite3_bind_text(statement, 5, hub, -1, SQLITE_STATIC);
    int failed = sqlite3_step(statement) != SQLITE_DONE || sqlite3_changes(db) != expected[i];
    sqlite3_finalize(statement);
    if (failed) return -1;
  }
  if (set_app_field(db, title, "DISPLAYLOCATION", location, NULL) ||
      set_app_field(db, title, "CATEGORY_TYPE", category, NULL) || set_app_field(db, title, "HUBAPP_URI", 0, hub))
    return -1;
  for (int i = 0; i < count; ++i) {
    char sql[160];
    snprintf(sql, sizeof sql, "UPDATE tbl_iconinfo_%s SET dispLocation = ?, hubAppUri = ? WHERE titleId = ?", users[i]);
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(statement, 1, location);
    sqlite3_bind_text(statement, 2, hub, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 3, title, -1, SQLITE_STATIC);
    int failed = sqlite3_step(statement) != SQLITE_DONE;
    sqlite3_finalize(statement);
    if (failed) return -1;
  }
  return write_concepts(db, title, location, users, count);
}

// Moves `title` to tab `location` on every profile, after backing up both databases. Returns the HTTP status;
// `message` explains a failure.
static int move_app(const char* title, int location, char* backup, size_t backup_size, char* message, size_t message_size) {
  sqlite3* db = open_db(1);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return 500; }
  int status = 500, began = 0, media = location == MEDIA_TAB, from = 0;
  {
    sqlite3_stmt* statement;
    if (sqlite3_prepare_v2(db, "SELECT dispLocation, appType, contentLocation FROM tbl_contentinfo WHERE titleId = ? AND "
                           "json_valid(AppInfoJson)", -1, &statement, NULL) != SQLITE_OK) {
      snprintf(message, message_size, "Could not read the app (%s).", sqlite3_errmsg(db));
      goto done;
    }
    sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
    int found = sqlite3_step(statement) == SQLITE_ROW;
    from = found ? sqlite3_column_int(statement, 0) : 0;
    int type = found ? sqlite3_column_int(statement, 1) : 0, place = found ? sqlite3_column_int(statement, 2) : 0;
    sqlite3_finalize(statement);
    if (!found) { snprintf(message, message_size, "No such app."); status = 404; goto done; }
    if (from != (media ? GAMES_TAB : MEDIA_TAB)) {
      snprintf(message, message_size, "That app changed since it was read. Open it again.");
      status = 409;
      goto done;
    }
    if (!can_move(title, type, place)) {
      snprintf(message, message_size, "Only homebrew and emulators can move between tabs.");
      status = 400;
      goto done;
    }
  }
  static char users[MAX_GAMES][16];
  int count = icon_users(db, users);
  if (count < 0) { snprintf(message, message_size, "Could not read the profiles (%s).", sqlite3_errmsg(db)); goto done; }
  if (check_and_backup(db, backup, backup_size, message, message_size) || attach_appinfo(db, message, message_size) ||
      backup_appinfo(db, backup, message, message_size)) goto done;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The database is busy (%s). Try again.", sqlite3_errmsg(db));
    goto done;
  }
  began = 1;
  int failed = record_move(db, title, from, location, users, count) || write_move(db, title, location, users, count);
  // A pin is a place on the tab the app just left.
  for (int i = 0; !failed && i < count; ++i) failed = forget_pin(db, users[i], title);
  if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The write failed (%s). Nothing changed.", sqlite3_errmsg(db));
    goto done;
  }
  began = 0;
  status = 200;
done:
  if (began) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
  sqlite3_close(db);
  return status;
}

// ---- Moving system tiles, and showing Welcome ----
//
// The Media Gallery can move between the Games and Media tabs (the PS Store only hides). The system's own tiles keep their tab
// as two bits of dispLocation, as Art Swap and PSGFX found: 0x02 Games and 0x04 Media (the Store 146 on Games and 148
// on Media, as App Library is; the Gallery 154 and 156), and 2048 above that on tbl_concepticoninfo_<user>. Moving
// sets one bit and clears the other wherever a tab is kept: tbl_iconinfo_<user>, tbl_concepticoninfo_<user>,
// tbl_contentinfo (and DISPLAYLOCATION in AppInfoJson), tbl_conceptmetadata (and display_location in ConceptInfoJson),
// and appinfo.db's tbl_appinfo (DISPLAYLOCATION) and tbl_conceptinfo (display_location). A value with neither bit, a
// tile the system keeps off both tabs, is left alone. The Game Library and App Library never move.
//
// The system hides a tile of its own with bit 0x01 of the same values and visible = 0 (Welcome and PS Plus are 147,
// one above the Store's 146). Showing Welcome clears that bit everywhere and sets visible on every profile; hiding it
// again sets the bit and clears visible. Measured on one console (13.60): Welcome may open nothing on a firmware
// that retired it.
//
// Art Swap wrote its changes only at its Apply changes, and the console put the tiles back on a restart or out of
// rest mode. lineup_system, in both databases, keeps for each tile the bits to set and to clear (and, for Welcome,
// that it is shown), and lineup_move_sys_* triggers write them again whenever the system writes one of those places
// with a bit that should be clear. A tile back as the system had it leaves lineup_system, and the last one takes the
// tables and triggers with it. JSON is left to restore_system, when Lineup starts.
#define BIT_HIDDEN 1
#define BIT_GAMES 2
#define BIT_MEDIA 4
#define WELCOME_TITLE "NPXS40063"

// Only the Media Gallery moves; the PS Store only hides.
static int is_system_movable(const char* title) { return !strcmp(title, GALLERY_TITLE); }

// A place the system keeps a tile's tab in. `table` and `name` have %s for the user on a profile's table. `key` is
// its column for the tile, matched with lineup_system's column `by` (the title or its concept); `value` holds the
// tab; `field`, on appinfo.db's key-value tables, is the row's key.
typedef struct { const char* schema; const char* table; const char* key; const char* by; const char* value;
                 const char* field; const char* name; } SystemPlace;

static const SystemPlace SYSTEM_PLACES[] = {
  {"main", "tbl_contentinfo", "titleId", "titleId", "dispLocation", NULL, "c"},
  {"main", "tbl_conceptmetadata", "localConceptId", "concept", "dispLocation", NULL, "m"},
  {"info", "tbl_appinfo", "titleId", "titleId", "val", "DISPLAYLOCATION", "a"},
  {"info", "tbl_conceptinfo", "local_concept_id", "concept", "val", "display_location", "k"},
  {"main", "tbl_iconinfo_%s", "titleId", "titleId", "dispLocation", NULL, "icon_%s"},
  {"main", "tbl_concepticoninfo_%s", "localConceptId", "concept", "dispLocation", NULL, "kicon_%s"},
};
#define SYSTEM_SHARED 4  // the first four are the console's; the others, each profile's
#define SYSTEM_PLACE_COUNT ((int)(sizeof SYSTEM_PLACES / sizeof SYSTEM_PLACES[0]))
#define SYSTEM_ICON 4    // tbl_iconinfo_<user>, which has visible too

// `place`'s table for `user` into `table`, and whether the firmware has its columns.
static int system_place_ready(sqlite3* db, const SystemPlace* place, const char* user, char* table, size_t size) {
  snprintf(table, size, place->table, user);
  return has_column(db, place->schema, table, place->key) && has_column(db, place->schema, table, place->value) &&
         (!place->field || has_column(db, place->schema, table, "key"));
}

// `field` as a condition on the row (`prefix` NEW. in a trigger), or nothing.
static void field_condition(const SystemPlace* place, const char* prefix, char* out, size_t size) {
  if (place->field) snprintf(out, size, "%skey = '%s' AND ", prefix, place->field);
  else *out = 0;
}

// The triggers missing on the places the firmware has (create = 0), or creates them (create = 1: inside a transaction,
// with appinfo.db attached as info and lineup_system in both). Returns how many were missing, or -1.
static int system_triggers(sqlite3* db, char users[][16], int count, int create) {
  int missing = 0;
  static char sql[2048];
  for (int p = 0; p < SYSTEM_PLACE_COUNT; ++p)
    for (int u = 0; u < (p < SYSTEM_SHARED ? 1 : count); ++u) {
      const SystemPlace* place = &SYSTEM_PLACES[p];
      const char* user = p < SYSTEM_SHARED ? "" : users[u];
      char table[64], name[64], insert[96], update[96];
      if (!system_place_ready(db, place, user, table, sizeof table)) continue;
      snprintf(name, sizeof name, place->name, user);
      // Showing Welcome: the system sets visible back to 0 at a restart, as it does for what Lineup hides.
      if (p == SYSTEM_ICON && has_column(db, "main", table, "visible")) {
        snprintf(insert, sizeof insert, "lineup_move_sys_vi_%s", user);
        snprintf(update, sizeof update, "lineup_move_sys_vu_%s", user);
        if (!has_object_in(db, "main", insert) || !has_object_in(db, "main", update)) {
          ++missing;
          for (int kind = 0; create && kind < 2; ++kind)
            if (snprintf(sql, sizeof sql, "CREATE TRIGGER IF NOT EXISTS main.%s AFTER %s ON %s WHEN NEW.visible = 0 AND "
                         "EXISTS (SELECT 1 FROM lineup_system WHERE titleId = NEW.titleId AND visible = 1) BEGIN UPDATE "
                         "%s SET visible = 1 WHERE titleId = NEW.titleId; END;", kind ? update : insert,
                         kind ? "UPDATE OF visible" : "INSERT", table, table) >= (int)sizeof sql ||
                sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
        }
      }
      snprintf(insert, sizeof insert, "lineup_move_sys_i_%s", name);
      snprintf(update, sizeof update, "lineup_move_sys_u_%s", name);
      if (has_object_in(db, place->schema, insert) && has_object_in(db, place->schema, update)) continue;
      ++missing;
      if (!create) continue;
      char when[64], where[64], set[160], clear[160];
      field_condition(place, "NEW.", when, sizeof when);
      field_condition(place, "", where, sizeof where);
      snprintf(set, sizeof set, "(SELECT setbits FROM lineup_system WHERE %s = NEW.%s)", place->by, place->key);
      snprintf(clear, sizeof clear, "(SELECT clearbits FROM lineup_system WHERE %s = NEW.%s)", place->by, place->key);
      for (int kind = 0; kind < 2; ++kind) {
        // When the system writes a bit the tile should not have, it gets its own back; anything else stays.
        char event[64];
        snprintf(event, sizeof event, kind ? "UPDATE OF %s" : "INSERT", place->value);
        if (snprintf(sql, sizeof sql,
              "CREATE TRIGGER IF NOT EXISTS %s.%s AFTER %s ON %s WHEN %sEXISTS (SELECT 1 FROM lineup_system s WHERE "
              "s.%s = NEW.%s AND (CAST(NEW.%s AS INTEGER) & s.clearbits) <> 0) BEGIN UPDATE %s SET %s = "
              "(CAST(%s AS INTEGER) | %s) & ~%s WHERE %s%s = NEW.%s AND (CAST(%s AS INTEGER) & %s) <> 0; END;",
              place->schema, kind ? update : insert, event, table, when, place->by, place->key, place->value, table,
              place->value, place->value, set, clear, where, place->key, place->key, place->value, clear) >=
                (int)sizeof sql ||
            sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
      }
    }
  return missing;
}

// Inside a transaction, with appinfo.db attached as info: removes the system tiles' triggers and tables, in both,
// and the MyHome rows kept in lineup_home_<user>.
static int drop_system(sqlite3* db) {
  static char names[5 * MAX_GAMES + 16][96];
  int count = 0;
  for (int kind = 0; kind < 3; ++kind) {
    char sql[160];
    snprintf(sql, sizeof sql, "SELECT name FROM %s.sqlite_master WHERE type = '%s' AND name GLOB '%s'",
             kind == 1 ? "info" : "main", kind == 2 ? "table" : "trigger",
             kind == 2 ? "lineup_home_*" : "lineup_move_sys_*");
    sqlite3_stmt* statement;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
    while (count < (int)(sizeof names / sizeof names[0]) && sqlite3_step(statement) == SQLITE_ROW)
      snprintf(names[count++], sizeof names[0], "%s %s.\"%s\"", kind == 2 ? "TABLE" : "TRIGGER",
               kind == 1 ? "info" : "main", (const char*)sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
  }
  for (int i = 0; i < count; ++i) {
    char sql[128];
    snprintf(sql, sizeof sql, "DROP %s", names[i]);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
  }
  return sqlite3_exec(db, "DROP TABLE IF EXISTS main.lineup_system; DROP TABLE IF EXISTS info.lineup_system;", NULL,
                      NULL, NULL) == SQLITE_OK ? 0 : -1;
}

// System tile `title`'s value in tbl_contentinfo into `location`, and its concept into `concept`. Returns -1 on
// failure, 0 when there is no such tile, 1.
static int system_tile(sqlite3* db, const char* title, long long* location, char* concept, size_t size) {
  int concepts = has_column(db, "main", "tbl_contentinfo", "localConceptId");
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, concepts ? "SELECT dispLocation, ifnull(localConceptId, '') FROM tbl_contentinfo WHERE "
                         "titleId = ?" : "SELECT dispLocation, '' FROM tbl_contentinfo WHERE titleId = ?", -1, &statement,
                         NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  int step = sqlite3_step(statement);
  if (step == SQLITE_ROW) {
    *location = sqlite3_column_int64(statement, 0);
    snprintf(concept, size, "%s", (const char*)sqlite3_column_text(statement, 1));
  }
  sqlite3_finalize(statement);
  return step == SQLITE_ROW ? 1 : step == SQLITE_DONE ? 0 : -1;
}

// The tab bit of a value: BIT_GAMES, BIT_MEDIA, or 0 off both.
static int tab_bit(long long location) { return location & BIT_GAMES ? BIT_GAMES : location & BIT_MEDIA ? BIT_MEDIA : 0; }

// Inside a transaction, with appinfo.db attached as info: sets bits `set` and clears `clear` on `title` in every place
// the firmware has, where the value has a bit of `only`.
static int write_system(sqlite3* db, const char* title, const char* concept, int set, int clear, int only,
                        char users[][16], int count) {
  for (int p = 0; p < SYSTEM_PLACE_COUNT; ++p)
    for (int u = 0; u < (p < SYSTEM_SHARED ? 1 : count); ++u) {
      const SystemPlace* place = &SYSTEM_PLACES[p];
      const int by_concept = !strcmp(place->by, "concept");
      char table[64], where[64];
      static char sql[512];
      if ((by_concept && !*concept) ||
          !system_place_ready(db, place, p < SYSTEM_SHARED ? "" : users[u], table, sizeof table)) continue;
      field_condition(place, "", where, sizeof where);
      snprintf(sql, sizeof sql, "UPDATE %s.%s SET %s = (CAST(%s AS INTEGER) | %d) & ~%d WHERE %s%s = ?2 AND "
               "(CAST(%s AS INTEGER) & %d) <> 0", place->schema, table, place->value, place->value, set, clear, where,
               place->key, place->value, only);
      if (run_values(db, sql, 0, by_concept ? concept : title, NULL) < 0) return -1;
    }
  // The JSON copies: only the tab's value changes.
  if (has_column(db, "main", "tbl_contentinfo", "AppInfoJson") &&
      run_values(db, "UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', dispLocation) "
                 "WHERE titleId = ?2 AND json_valid(AppInfoJson) AND json_type(AppInfoJson, '$.DISPLAYLOCATION') IS NOT "
                 "NULL AND AppInfoJson ->> '$.DISPLAYLOCATION' IS NOT dispLocation", 0, title, NULL) < 0) return -1;
  if (has_column(db, "main", "tbl_contentinfo", "AppInfoJson") &&
      run_values(db, "UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.field_list[' || (SELECT e.key FROM "
                 "json_each(" SAFE_JSON("AppInfoJson") ", '$.field_list') e WHERE e.value ->> '$.key' = 'DISPLAYLOCATION' "
                 "LIMIT 1) || '].data', dispLocation) WHERE titleId = ?2 AND json_valid(AppInfoJson) AND EXISTS (SELECT 1 "
                 "FROM json_each(" SAFE_JSON("AppInfoJson") ", '$.field_list') e WHERE e.value ->> '$.key' = "
                 "'DISPLAYLOCATION' AND e.value ->> '$.data' IS NOT dispLocation)", 0, title, NULL) < 0) return -1;
  if (*concept && fix_concept_json(db, concept)) return -1;
  // Off the Games row, a tile leaves each profile's MyHome group, which places the system's tiles there; the rest
  // keep their order. lineup_home_<user> keeps its row, every column of it, with idx its place counted from 1, for
  // return_home.
  for (int u = 0; set == BIT_MEDIA && u < count; ++u) {
    char table[64], home[64], sql[300];
    snprintf(table, sizeof table, "tbl_group_%s", users[u]);
    snprintf(home, sizeof home, "lineup_home_%s", users[u]);
    if (!has_column(db, "main", table, "itemId") || !has_column(db, "main", table, "groupName") ||
        !has_column(db, "main", table, "idx")) continue;
    snprintf(sql, sizeof sql, "SELECT 1 FROM %s WHERE groupName = 'MyHome' AND itemId = ?1", table);
    int found = finds_row(db, sql, title);
    if (found < 0) return -1;
    if (found) {
      snprintf(sql, sizeof sql, "CREATE TABLE IF NOT EXISTS main.%s AS SELECT * FROM %s WHERE 0", home, table);
      if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
      snprintf(sql, sizeof sql, "DELETE FROM %s WHERE itemId = ?2", home);
      if (run_values(db, sql, 0, title, NULL) < 0) return -1;
      snprintf(sql, sizeof sql, "INSERT INTO %s SELECT * FROM %s WHERE groupName = 'MyHome' AND itemId = ?2", home, table);
      if (run_values(db, sql, 0, title, NULL) < 0) return -1;
      snprintf(sql, sizeof sql, "UPDATE %1$s SET idx = (SELECT COUNT(*) FROM %2$s g WHERE g.groupName = 'MyHome' AND "
               "g.idx <= %1$s.idx) WHERE itemId = ?2", home, table);
      if (run_values(db, sql, 0, title, NULL) < 0) return -1;
    }
    snprintf(sql, sizeof sql, "DELETE FROM %s WHERE groupName = 'MyHome' AND itemId = ?2", table);
    int removed = run_values(db, sql, 0, title, NULL);
    if (removed < 0) return -1;
    snprintf(sql, sizeof sql, "UPDATE %1$s SET idx = (SELECT COUNT(*) FROM %1$s g WHERE g.groupName = 'MyHome' AND "
             "g.idx <= %1$s.idx) WHERE groupName = 'MyHome'", table);
    if (removed && run_values(db, sql, 0, NULL, NULL) < 0) return -1;
  }
  return 0;
}

// Inside a transaction: puts `title`, back on the Games row, in each profile's MyHome group where it left (its row
// kept in lineup_home_<user>), or last when nothing was kept, and lets go of the kept row. The rest keep their order.
static int return_home(sqlite3* db, const char* title, char users[][16], int count) {
  for (int u = 0; u < count; ++u) {
    char table[64], home[64], sql[300];
    snprintf(table, sizeof table, "tbl_group_%s", users[u]);
    snprintf(home, sizeof home, "lineup_home_%s", users[u]);
    if (!has_column(db, "main", table, "itemId") || !has_column(db, "main", table, "groupName") ||
        !has_column(db, "main", table, "idx")) continue;
    snprintf(sql, sizeof sql, "SELECT 1 FROM %s WHERE groupName = 'MyHome' AND itemId = ?1", table);
    int there = finds_row(db, sql, title), kept = 0;
    if (has_object_in(db, "main", home)) {
      snprintf(sql, sizeof sql, "SELECT 1 FROM %s WHERE itemId = ?1", home);
      kept = finds_row(db, sql, title);
    }
    if (there < 0 || kept < 0) return -1;
    if (!there) {
      if (kept) {
        snprintf(sql, sizeof sql, "UPDATE %1$s SET idx = idx + 1 WHERE groupName = 'MyHome' AND idx >= (SELECT idx FROM "
                 "%2$s WHERE itemId = ?2)", table, home);
        if (run_values(db, sql, 0, title, NULL) < 0) return -1;
        snprintf(sql, sizeof sql, "INSERT INTO %s SELECT * FROM %s WHERE itemId = ?2", table, home);
      } else {
        snprintf(sql, sizeof sql, "INSERT INTO %1$s (itemId, groupName, idx) VALUES (?2, 'MyHome', (SELECT ifnull(MAX(idx), "
                 "0) + 1 FROM %1$s WHERE groupName = 'MyHome'))", table);
      }
      if (run_values(db, sql, 0, title, NULL) < 0) return -1;
      snprintf(sql, sizeof sql, "UPDATE %1$s SET idx = (SELECT COUNT(*) FROM %1$s g WHERE g.groupName = 'MyHome' AND "
               "g.idx <= %1$s.idx) WHERE groupName = 'MyHome'", table);
      if (run_values(db, sql, 0, NULL, NULL) < 0) return -1;
    }
    if (!kept) continue;
    snprintf(sql, sizeof sql, "DELETE FROM %s WHERE itemId = ?2", home);
    if (run_values(db, sql, 0, title, NULL) < 0) return -1;
    snprintf(sql, sizeof sql, "SELECT 1 FROM %s LIMIT 1", home);
    int left = finds_row(db, sql, NULL);
    snprintf(sql, sizeof sql, "DROP TABLE main.%s", home);
    if (left < 0 || (!left && sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK)) return -1;
  }
  return 0;
}

// Inside a transaction: `title`'s visible on every profile.
static int system_visible(sqlite3* db, const char* title, int visible, char users[][16], int count) {
  for (int u = 0; u < count; ++u) {
    char table[64], sql[160];
    snprintf(table, sizeof table, "tbl_iconinfo_%s", users[u]);
    if (!has_column(db, "main", table, "visible")) continue;
    snprintf(sql, sizeof sql, "UPDATE %s SET visible = ?1 WHERE titleId = ?2 AND visible IS NOT ?1", table);
    if (run_values(db, sql, visible, title, NULL) < 0) return -1;
  }
  return 0;
}

#define SYSTEM_TABLES \
  "CREATE TABLE IF NOT EXISTS main.lineup_system (titleId TEXT PRIMARY KEY, concept TEXT, setbits INTEGER, clearbits " \
  "INTEGER, home INTEGER, visible INTEGER);" \
  "CREATE TABLE IF NOT EXISTS info.lineup_system (titleId TEXT PRIMARY KEY, concept TEXT, setbits INTEGER, clearbits " \
  "INTEGER);"

// Inside a transaction, with appinfo.db attached as info: keeps `title`'s bits in lineup_system (visible 1 to keep it
// shown), with `home` the tab bit it started on, and makes the triggers.
static int keep_system(sqlite3* db, const char* title, const char* concept, int set, int clear, int home, int visible,
                       char users[][16], int count) {
  char sql[200];
  if (sqlite3_exec(db, SYSTEM_TABLES, NULL, NULL, NULL) != SQLITE_OK) return -1;
  snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO main.lineup_system VALUES (?3, ?2, %d, %d, %d, %d)", set, clear, home,
           visible);
  if (run_values(db, sql, 0, concept, title) < 0) return -1;
  snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO info.lineup_system VALUES (?3, ?2, %d, %d)", set, clear);
  if (run_values(db, sql, 0, concept, title) < 0) return -1;
  return system_triggers(db, users, count, 1) < 0 ? -1 : 0;
}

// Inside a transaction, with appinfo.db attached as info: lets go of `title`; the last one takes the tables and
// triggers.
static int forget_system(sqlite3* db, const char* title) {
  if (!has_object_in(db, "main", "lineup_system")) return 0;
  if (run_title(db, "DELETE FROM main.lineup_system WHERE titleId = ?1", NULL, title) < 0 ||
      (has_object_in(db, "info", "lineup_system") &&
       run_title(db, "DELETE FROM info.lineup_system WHERE titleId = ?1", NULL, title) < 0)) return -1;
  int left = finds_row(db, "SELECT 1 FROM main.lineup_system LIMIT 1", NULL);
  return left < 0 ? -1 : left ? 0 : drop_system(db);
}

// The home bit lineup_system keeps for `title`, or `otherwise`.
static int system_home(sqlite3* db, const char* title, int otherwise) {
  if (!has_object_in(db, "main", "lineup_system")) return otherwise;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT home FROM main.lineup_system WHERE titleId = ?", -1, &statement, NULL) != SQLITE_OK)
    return otherwise;
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  int home = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : otherwise;
  sqlite3_finalize(statement);
  return home;
}

// Opens app.db for a system tile's change: reads `title`, its profiles and backs up both databases, then begins.
// Returns the database, or NULL with *status and `message` set.
static sqlite3* begin_system(const char* title, long long* location, char* concept, size_t concept_size,
                             char users[][16], int* count, char* backup, size_t backup_size, char* message,
                             size_t message_size, int* status) {
  *status = 500;
  sqlite3* db = open_db(1);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return NULL; }
  int found = system_tile(db, title, location, concept, concept_size);
  if (found <= 0) {
    if (found < 0) snprintf(message, message_size, "Could not read the tile (%s).", sqlite3_errmsg(db));
    else { snprintf(message, message_size, "No such tile."); *status = 404; }
  } else if ((*count = icon_users(db, users)) < 0) {
    snprintf(message, message_size, "Could not read the profiles (%s).", sqlite3_errmsg(db));
  } else if (!check_and_backup(db, backup, backup_size, message, message_size) &&
             !attach_appinfo(db, message, message_size) && !backup_appinfo(db, backup, message, message_size)) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK) return db;
    snprintf(message, message_size, "The database is busy (%s). Try again.", sqlite3_errmsg(db));
  }
  sqlite3_close(db);
  return NULL;
}

// Commits a system tile's change begun by begin_system, or rolls it back. Returns the HTTP status.
static int end_system(sqlite3* db, int failed, char* message, size_t message_size) {
  if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The write failed (%s). Nothing changed.", sqlite3_errmsg(db));
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    return 500;
  }
  sqlite3_close(db);
  return 200;
}

// Moves system tile `title` (the Media Gallery) to the Media tab or back to Games, on every profile,
// after backing up both databases. Returns the HTTP status; `message` explains a failure.
static int move_system(const char* title, int media, char* backup, size_t backup_size, char* message,
                       size_t message_size) {
  if (!is_system_movable(title)) { snprintf(message, message_size, "Only the Media Gallery moves."); return 400; }
  static char users[MAX_GAMES][16];
  char concept[96] = "";
  long long location = 0;
  int count = 0, status, bit = media ? BIT_MEDIA : BIT_GAMES;
  sqlite3* db = begin_system(title, &location, concept, sizeof concept, users, &count, backup, backup_size, message,
                             message_size, &status);
  if (!db) return status;
  int from = tab_bit(location);
  if (from != (media ? BIT_GAMES : BIT_MEDIA)) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    snprintf(message, message_size, "That tile changed since it was read. Open it again.");
    return 409;
  }
  int home = system_home(db, title, from);
  // Back in MyHome first: forgetting the last tile drops what was kept of it.
  int failed = (bit == BIT_GAMES && return_home(db, title, users, count)) ||
               (home == bit ? forget_system(db, title)
                            : keep_system(db, title, concept, bit, 6 - bit, home, 0, users, count)) ||
               write_system(db, title, concept, bit, 6 - bit, 6 - bit, users, count);
  // A pin is a place on the tab the tile just left.
  for (int i = 0; !failed && i < count; ++i) failed = forget_pin(db, users[i], title);
  return end_system(db, failed, message, message_size);
}

// Shows Welcome on every profile, or hides it again as the system had it, after backing up both databases. Returns
// the HTTP status; `message` explains a failure.
static int show_welcome(int show, char* backup, size_t backup_size, char* message, size_t message_size) {
  static char users[MAX_GAMES][16];
  char concept[96] = "";
  long long location = 0;
  int count = 0, status;
  sqlite3* db = begin_system(WELCOME_TITLE, &location, concept, sizeof concept, users, &count, backup, backup_size,
                             message, message_size, &status);
  if (!db) return status;
  int kept = finds_row(db, has_object_in(db, "main", "lineup_system")
                       ? "SELECT 1 FROM main.lineup_system WHERE titleId = '" WELCOME_TITLE "'" : "SELECT 1 WHERE 0", NULL);
  if (kept < 0 || kept == show || !tab_bit(location)) {
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    if (kept < 0) { snprintf(message, message_size, "Could not read the tile."); return 500; }
    snprintf(message, message_size, !tab_bit(location) ? "The system keeps Welcome off both tabs here."
             : show ? "Welcome is already shown." : "Lineup did not show Welcome.");
    return 409;
  }
  int failed = show ? keep_system(db, WELCOME_TITLE, concept, 0, BIT_HIDDEN, 0, 1, users, count) ||
                      write_system(db, WELCOME_TITLE, concept, 0, BIT_HIDDEN, BIT_HIDDEN, users, count) ||
                      system_visible(db, WELCOME_TITLE, 1, users, count)
                    : forget_system(db, WELCOME_TITLE) ||
                      write_system(db, WELCOME_TITLE, concept, BIT_HIDDEN, 0, BIT_GAMES | BIT_MEDIA, users, count) ||
                      system_visible(db, WELCOME_TITLE, 0, users, count);
  return end_system(db, failed, message, message_size);
}

// 1 when a kept tile is off its bits or visible in some place, its JSON included; 0 when not, -1 on failure.
// appinfo.db is attached as info.
static int system_stale(sqlite3* db, char users[][16], int count) {
  for (int p = 0; p < SYSTEM_PLACE_COUNT; ++p)
    for (int u = 0; u < (p < SYSTEM_SHARED ? 1 : count); ++u) {
      const SystemPlace* place = &SYSTEM_PLACES[p];
      char table[64], where[64];
      static char sql[512];
      if (!system_place_ready(db, place, p < SYSTEM_SHARED ? "" : users[u], table, sizeof table)) continue;
      field_condition(place, "x.", where, sizeof where);
      snprintf(sql, sizeof sql, "SELECT 1 FROM %1$s.%2$s x JOIN %1$s.lineup_system s ON s.%3$s = x.%4$s WHERE %5$s"
               "(CAST(x.%6$s AS INTEGER) & s.clearbits) <> 0 LIMIT 1", place->schema, table, place->by, place->key,
               where, place->value);
      int found = finds_row(db, sql, NULL);
      if (!found && p == SYSTEM_ICON && has_column(db, "main", table, "visible")) {
        snprintf(sql, sizeof sql, "SELECT 1 FROM %s x JOIN main.lineup_system s ON s.titleId = x.titleId WHERE "
                 "s.visible = 1 AND x.visible = 0 LIMIT 1", table);
        found = finds_row(db, sql, NULL);
      }
      if (found) return found;
    }
  int found = 0;
  if (has_column(db, "main", "tbl_contentinfo", "AppInfoJson"))
    found = finds_row(db, "SELECT 1 FROM tbl_contentinfo c JOIN main.lineup_system s ON s.titleId = c.titleId WHERE "
                      "json_valid(c.AppInfoJson) AND ifnull(" APP_FIELD("c.AppInfoJson", "DISPLAYLOCATION") ", "
                      "c.dispLocation) IS NOT c.dispLocation LIMIT 1", NULL);
  if (!found && has_column(db, "main", "tbl_conceptmetadata", "ConceptInfoJson"))
    found = finds_row(db, "SELECT 1 FROM tbl_conceptmetadata x JOIN main.lineup_system s ON s.concept = x.localConceptId "
                      "JOIN json_each(" SAFE_JSON("x.ConceptInfoJson") ", '$.field_list') e WHERE "
                      "json_valid(x.ConceptInfoJson) AND e.value ->> '$.key' = 'display_location' AND "
                      "e.value ->> '$.data' IS NOT x.dispLocation LIMIT 1", NULL);
  return found;
}

// When Lineup starts: puts a tile an earlier test build moved, and that no longer moves (the PS Store), back on the
// tab it started on, and lets go of it, after a backup. Returns how many it put back, or -1.
static int release_unmovable(void) {
  sqlite3* db = open_db(1);
  if (!db) return -1;
  if (!has_object(db, "lineup_system")) { sqlite3_close(db); return 0; }
  static char users[MAX_GAMES][16], titles[8][16], concepts[8][96];
  static int homes[8];
  char backup[256] = "", message[300] = "";
  int count = icon_users(db, users), tiles = 0;
  sqlite3_stmt* statement;
  int failed = count < 0 || sqlite3_prepare_v2(db, "SELECT titleId, ifnull(concept, ''), home FROM main.lineup_system "
                                               "WHERE titleId NOT IN ('" GALLERY_TITLE "', '" WELCOME_TITLE "')", -1,
                                               &statement, NULL) != SQLITE_OK;
  while (!failed && tiles < 8 && sqlite3_step(statement) == SQLITE_ROW) {
    snprintf(titles[tiles], sizeof titles[0], "%s", (const char*)sqlite3_column_text(statement, 0));
    snprintf(concepts[tiles], sizeof concepts[0], "%s", (const char*)sqlite3_column_text(statement, 1));
    homes[tiles++] = sqlite3_column_int(statement, 2);
  }
  if (count >= 0 && !failed) sqlite3_finalize(statement);
  if (!failed && tiles) {
    failed = attach_appinfo(db, message, sizeof message) ||
             check_and_backup(db, backup, sizeof backup, message, sizeof message) ||
             backup_appinfo(db, backup, message, sizeof message) ||
             sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK;
    for (int i = 0; !failed && i < tiles; ++i) {
      int home = homes[i];
      failed = forget_system(db, titles[i]) ||
               ((home == BIT_GAMES || home == BIT_MEDIA) &&
                write_system(db, titles[i], concepts[i], home, 6 - home, 6 - home, users, count));
    }
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      LOG("tiles that no longer move not put back: %s", *message ? message : sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      failed = 1;
    } else LOG("tiles that no longer move put back: %d, backup %s", tiles, backup);
  }
  sqlite3_close(db);
  return failed ? -1 : tiles;
}

// When Lineup starts: writes the kept system tiles again wherever the triggers could not (the JSON, a profile made
// since, a write while they were missing), and gives every place its triggers, after a backup. Returns how many tiles
// it wrote, or -1.
static int restore_system(void) {
  sqlite3* db = open_db(1);
  if (!db) return -1;
  if (!has_object(db, "lineup_system")) { sqlite3_close(db); return 0; }
  static char users[MAX_GAMES][16], titles[8][16], concepts[8][96];
  static int sets[8], clears[8], shown[8];
  char backup[256] = "", message[300] = "";
  int count = icon_users(db, users), tiles = 0;
  int failed = count < 0 || attach_appinfo(db, message, sizeof message);
  int stale = failed ? 0 : system_stale(db, users, count);
  int missing = failed || stale < 0 ? 0 : system_triggers(db, users, count, 0);
  failed = failed || stale < 0 || missing < 0;
  if (!failed && (stale || missing)) {
    sqlite3_stmt* statement;
    failed = sqlite3_prepare_v2(db, "SELECT titleId, ifnull(concept, ''), setbits, clearbits, ifnull(visible, 0) FROM "
                                "main.lineup_system", -1, &statement, NULL) != SQLITE_OK;
    while (!failed && tiles < 8 && sqlite3_step(statement) == SQLITE_ROW) {
      snprintf(titles[tiles], sizeof titles[0], "%s", (const char*)sqlite3_column_text(statement, 0));
      snprintf(concepts[tiles], sizeof concepts[0], "%s", (const char*)sqlite3_column_text(statement, 1));
      sets[tiles] = sqlite3_column_int(statement, 2);
      clears[tiles] = sqlite3_column_int(statement, 3);
      shown[tiles++] = sqlite3_column_int(statement, 4);
    }
    if (!failed) sqlite3_finalize(statement);
    failed = failed || check_and_backup(db, backup, sizeof backup, message, sizeof message) ||
             backup_appinfo(db, backup, message, sizeof message) ||
             sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK ||
             system_triggers(db, users, count, 1) < 0;
    for (int i = 0; !failed && i < tiles; ++i) {
      // Only what Lineup writes: a tab, or Welcome shown.
      int known = (sets[i] == BIT_GAMES && clears[i] == BIT_MEDIA) || (sets[i] == BIT_MEDIA && clears[i] == BIT_GAMES) ||
                  (sets[i] == 0 && clears[i] == BIT_HIDDEN);
      failed = !known || write_system(db, titles[i], concepts[i], sets[i], clears[i], clears[i], users, count) ||
               (shown[i] && system_visible(db, titles[i], 1, users, count));
    }
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      LOG("system tiles not restored: %s", *message ? message : sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      failed = 1;
    } else LOG("system tiles restored: %d, backup %s", tiles, backup);
  } else if (failed) LOG("system tiles not checked: %s", *message ? message : sqlite3_errmsg(db));
  sqlite3_close(db);
  return failed ? -1 : stale || missing ? tiles : 0;
}

// Puts the moved apps back on their tab wherever the triggers could not (AppInfoJson, a profile created after the
// move, a write while they were missing), and gives every profile its triggers, after a backup. Returns how many apps
// it changed, or -1.
static int restore_moved(void) {
  sqlite3* db = open_db(1);
  if (!db) return -1;
  if (!has_object(db, "lineup_moved")) { sqlite3_close(db); return 0; }
  static char users[MAX_GAMES][16], titles[MAX_GAMES][16];
  static int tabs[MAX_GAMES];
  char backup[256] = "", message[300] = "";
  Buffer sql = {0};
  int count = icon_users(db, users), stale = 0, complete = 0;
  int failed = count < 0 || attach_appinfo(db, message, sizeof message);
  if (!failed) {
    puts_(&sql, "SELECT m.titleId, m.tab FROM lineup_moved m JOIN tbl_contentinfo c ON c.titleId = m.titleId AND "
          "json_valid(c.AppInfoJson) WHERE c.dispLocation <> m.tab OR ifnull(" APP_FIELD("c.AppInfoJson", "DISPLAYLOCATION")
          ", m.tab) IS NOT m.tab "
          "OR EXISTS (SELECT 1 FROM info.tbl_appinfo a WHERE a.titleId = m.titleId AND a.key = 'DISPLAYLOCATION' AND "
          "a.val IS NOT m.tab)");
    for (int i = 0; i < count; ++i)
      putf(&sql, " OR EXISTS (SELECT 1 FROM tbl_iconinfo_%s i WHERE i.titleId = m.titleId AND i.dispLocation IN (138, 188) "
           "AND i.dispLocation <> m.tab)",
           users[i]);
    // The concept places. Apps moved before Lineup kept them (1.8.0) are all written again once, which records them.
    if (!has_object_in(db, "main", "lineup_moved_concept") || !has_object_in(db, "info", "lineup_moved_concept")) {
      puts_(&sql, " OR 1");
    } else {
      if (has_column(db, "main", "tbl_contentinfo", "localConceptId"))
        puts_(&sql, " OR (NOT EXISTS (SELECT 1 FROM lineup_moved_concept k WHERE k.titleId = m.titleId) AND "
              "ifnull(c.localConceptId, '') <> '' AND NOT EXISTS (SELECT 1 FROM tbl_contentinfo o WHERE "
              "o.localConceptId = c.localConceptId AND o.titleId <> c.titleId))");
      if (has_columns(db, "main", "tbl_conceptmetadata", CONCEPT_META_COLUMNS)) {
        puts_(&sql, " OR EXISTS (SELECT 1 FROM lineup_moved_concept k JOIN tbl_conceptmetadata x ON x.localConceptId = "
              "k.concept WHERE k.titleId = m.titleId AND x.dispLocation IN (138, 188) AND x.dispLocation <> m.tab)");
        if (has_column(db, "main", "tbl_conceptmetadata", "ConceptInfoJson"))
          puts_(&sql, " OR EXISTS (SELECT 1 FROM lineup_moved_concept k JOIN tbl_conceptmetadata x ON x.localConceptId = "
                "k.concept AND json_valid(x.ConceptInfoJson) JOIN json_each(" SAFE_JSON("x.ConceptInfoJson") ", '$.field_list') e WHERE "
                "k.titleId = m.titleId AND e.value ->> '$.key' = 'display_location' AND e.value ->> '$.data' IS NOT "
                "x.dispLocation)");
      }
      if (has_columns(db, "info", "tbl_conceptinfo", CONCEPT_INFO_COLUMNS))
        puts_(&sql, " OR EXISTS (SELECT 1 FROM lineup_moved_concept k JOIN info.tbl_conceptinfo x ON x.local_concept_id = "
              "k.concept AND x.key = 'display_location' WHERE k.titleId = m.titleId AND x.val IS NOT m.tab)");
      for (int i = 0; i < count; ++i) {
        char table[64];
        snprintf(table, sizeof table, "tbl_concepticoninfo_%s", users[i]);
        char clause[512];  // longer than putf's line
        if (has_columns(db, "main", table, CONCEPT_ICON_COLUMNS) &&
            snprintf(clause, sizeof clause, " OR EXISTS (SELECT 1 FROM lineup_moved_concept k JOIN %s x ON "
                     "x.localConceptId = k.concept WHERE k.titleId = m.titleId AND x.dispLocation IN (1162, 2236) AND "
                     "x.dispLocation <> " CONCEPT_OF("m.tab") ")", table) < (int)sizeof clause)
          puts_(&sql, clause);
      }
    }
    sqlite3_stmt* statement;
    failed = !sql.data || sqlite3_prepare_v2(db, sql.data, -1, &statement, NULL) != SQLITE_OK;
    int step = SQLITE_DONE;
    while (!failed && stale < MAX_GAMES && (step = sqlite3_step(statement)) == SQLITE_ROW) {
      snprintf(titles[stale], sizeof titles[0], "%s", (const char*)sqlite3_column_text(statement, 0));
      tabs[stale++] = sqlite3_column_int(statement, 1);
    }
    if (!failed) {
      failed = step != SQLITE_ROW && step != SQLITE_DONE;
      sqlite3_finalize(statement);
    }
    // The concept triggers (lineup_move_k*) and the system tiles' (lineup_move_sys_*) are counted apart.
#define FIRST_MOVE_TRIGGERS "type = 'trigger' AND name GLOB 'lineup_move_*' AND name NOT GLOB 'lineup_move_k*' AND " \
  "name NOT GLOB 'lineup_move_sys_*'"
    char triggers[400];
    snprintf(triggers, sizeof triggers, "SELECT 1 WHERE (SELECT COUNT(*) FROM main.sqlite_master WHERE "
             FIRST_MOVE_TRIGGERS ") + (SELECT COUNT(*) FROM info.sqlite_master WHERE " FIRST_MOVE_TRIGGERS ") = %d",
             4 + 2 * count);
    complete = failed ? 0 : finds_row(db, triggers, NULL);
    int concepts = failed || complete < 0 ? 0 : concept_triggers(db, users, count, 0);
    failed = failed || complete < 0 || concepts < 0;
    if (concepts > 0) complete = 0;
  }
  if (!failed && (stale || !complete)) {
    failed = check_and_backup(db, backup, sizeof backup, message, sizeof message) ||
             backup_appinfo(db, backup, message, sizeof message) ||
             sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK || ensure_moves(db, users, count);
    for (int i = 0; !failed && i < stale; ++i)
      failed = record_concept(db, titles[i], tabs[i]) || write_move(db, titles[i], tabs[i], users, count);
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      LOG("moved apps not restored: %s", *message ? message : sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      failed = 1;
    } else LOG("moved apps restored: %d, backup %s", stale, backup);
  } else if (failed) LOG("moved apps not checked: %s", *message ? message : sqlite3_errmsg(db));
  free(sql.data);
  sqlite3_close(db);
  return failed ? -1 : stale;
}

// Unpins every game of `user` (/pins/off), or of every profile when `user` is NULL (/pins/uninstall, which also
// brings back what Lineup hid and lets go of the moves, leaving the apps where they are), after a backup. Returns
// the HTTP status; `message` explains a failure.
static int unpin(const char* user, char* backup, size_t backup_size, char* message, size_t message_size) {
  sqlite3* db = open_db(1);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return 500; }
  int status = 500;
  if (user && !table_exists(db, user)) { snprintf(message, message_size, "No such profile."); status = 404; goto done; }
  // Nothing to remove: no backup either.
  if (user && !has_object(db, "lineup_pin")) { status = 200; goto done; }
  int moves = !user && has_object(db, "lineup_moved");
  int tiles = !user && (has_object(db, "lineup_system") ||
                        finds_row(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name GLOB 'lineup_home_*'",
                                  NULL) > 0);
  if (check_and_backup(db, backup, backup_size, message, message_size) ||
      ((moves || tiles) && (attach_appinfo(db, message, message_size) ||
                            backup_appinfo(db, backup, message, message_size)))) goto done;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The database is busy (%s). Try again.", sqlite3_errmsg(db));
    goto done;
  }
  if ((user ? drop_pin_triggers(db, user) || forget_pins(db, user) || remove_pins_if_unused(db)
            : show_all(db) || remove_pins(db) || (moves && drop_moves(db)) || (tiles && drop_system(db))) ||
      sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The write failed (%s). Nothing changed.", sqlite3_errmsg(db));
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    goto done;
  }
  status = 200;
done:
  sqlite3_close(db);
  return status;
}

// ---- Profile pictures ----

// The picture of `user` (10 digits, already validated): avatar.png in profile_cache_dir/0x<id>, where the id is
// hex of 8 or 16 digits. Matches the whole value or its low 32 bits. Returns 0 with `out` set when a folder matches.
static int avatar_path(const char* user, char* out, size_t size) {
  unsigned long long id = strtoull(user, NULL, 10);
  DIR* dir = opendir(profile_cache_dir);
  int found = -1;
  for (struct dirent* entry; dir && found && (entry = readdir(dir));) {
    const char* name = entry->d_name;
    if (name[0] != '0' || (name[1] != 'x' && name[1] != 'X') || !isxdigit((unsigned char)name[2]) || strlen(name) > 18) continue;
    char* end;
    unsigned long long value = strtoull(name + 2, &end, 16);
    if (*end || (value != id && (value & 0xffffffffULL) != id)) continue;
    snprintf(out, size, "%s/%s/avatar.png", profile_cache_dir, name);
    found = 0;
  }
  if (dir) closedir(dir);
  return found;
}

// ---- Update files ----

// "1.2.0", or param.json's "01.002.000", as one comparable number; -1 when malformed.
static long version_number(const char* text) {
  unsigned major, minor, patch;
  char extra;
  if (!text || !*text || strspn(text, "0123456789.") != strlen(text) ||
      sscanf(text, "%u.%u.%u%c", &major, &minor, &patch, &extra) != 3 || major > 999 || minor > 999 || patch > 999)
    return -1;
  return (long)major * 1000000 + minor * 1000 + patch;
}

// The string value of "key" in a flat JSON text, copied into `out`; NULL when absent.
static const char* json_text(const char* json, const char* key, char* out, size_t size) {
  char quoted[64];
  snprintf(quoted, sizeof quoted, "\"%s\"", key);
  const char* p = strstr(json, quoted);
  if (!p) return NULL;
  for (p += strlen(quoted); isspace((unsigned char)*p); ++p) {}
  if (*p++ != ':') return NULL;
  while (isspace((unsigned char)*p)) ++p;
  if (*p++ != '"') return NULL;
  size_t length = strcspn(p, "\"");
  if (p[length] != '"' || length >= size) return NULL;
  memcpy(out, p, length);
  out[length] = 0;
  return out;
}

static int is_file(const char* dir, const char* name) {
  char path[512];
  struct stat info;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  return !stat(path, &info) && S_ISREG(info.st_mode);
}

// The version of an app folder ready to install (sce_sys/param.json for TITLE_ID, eboot.bin, lineupd.elf), or -1.
static long app_folder_version(const char* dir) {
  static char text[16 * 1024];
  char path[512], value[32];
  snprintf(path, sizeof path, "%s/sce_sys/param.json", dir);
  FILE* file = fopen(path, "rb");
  if (!file) return -1;
  size_t size = fread(text, 1, sizeof text - 1, file);
  fclose(file);
  text[size] = 0;
  if (!json_text(text, "titleId", value, sizeof value) || strcmp(value, TITLE_ID) ||
      !is_file(dir, "eboot.bin") || !is_file(dir, "lineupd.elf"))
    return -1;
  return json_text(text, "contentVersion", value, sizeof value) ? version_number(value) : -1;
}

static int is_dot(const char* name) { return !strcmp(name, ".") || !strcmp(name, ".."); }

// Removes a file, or a directory and everything in it; links are removed, never followed. 0 once it is gone.
static int remove_tree(const char* path) {
  struct stat info;
  if (lstat(path, &info)) return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(info.st_mode)) return unlink(path);
  DIR* dir = opendir(path);
  if (!dir) return -1;
  int failed = 0;
  // Passes until one removes nothing: removing entries while reading a directory may skip some.
  for (int removed = 1; removed && !failed;) {
    removed = 0;
    rewinddir(dir);
    for (struct dirent* entry; !failed && (entry = readdir(dir));) {
      if (is_dot(entry->d_name)) continue;
      char child[512];
      snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
      failed = remove_tree(child);
      removed = 1;
    }
  }
  closedir(dir);
  return failed || rmdir(path) ? -1 : 0;
}

// Copies a file to `to`, created or truncated. No fsync per file: callers sync() once when they are done.
static int copy_file(const char* from, const char* to) {
  static char chunk[64 * 1024];
  int in = open(from, O_RDONLY), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  int failed = out < 0;
  for (ssize_t count; !failed && (count = read(in, chunk, sizeof chunk)) != 0;)
    failed = count < 0 || write(out, chunk, (size_t)count) != count;
  // The console's own services read a title's files, so they are open to every user.
  if (!failed) failed = fchmod(out, 0777);
  if (in >= 0) close(in);
  if (out >= 0 && close(out)) failed = 1;
  return failed ? -1 : 0;
}

// Copies the directory `from` to `to`, which must not exist yet.
static int copy_tree(const char* from, const char* to) {
  DIR* dir = opendir(from);
  if (!dir || mkdir(to, 0777) || chmod(to, 0777)) {
    if (dir) closedir(dir);
    return -1;
  }
  int failed = 0;
  for (struct dirent* entry; !failed && (entry = readdir(dir));) {
    if (is_dot(entry->d_name)) continue;
    char source[512], target[512];
    struct stat info;
    snprintf(source, sizeof source, "%s/%s", from, entry->d_name);
    snprintf(target, sizeof target, "%s/%s", to, entry->d_name);
    if (stat(source, &info)) failed = -1;
    else if (S_ISDIR(info.st_mode)) failed = copy_tree(source, target);
    else if (S_ISREG(info.st_mode)) failed = copy_file(source, target);
  }
  closedir(dir);
  return failed ? -1 : 0;
}

// Makes the directory `to` hold what `from` holds, one file at a time: each file is written beside the old one
// as .<name>.new and renamed over it; then what `from` lacks is removed. `to` stays the same directory,
// so a mount of it (ShadowMountPlus mounts an app folder with nullfs) sees the new files.
static int swap_tree(const char* from, const char* to) {
  if (mkdir(to, 0777) && errno != EEXIST) return -1;
  DIR* dir = opendir(from);
  if (!dir) return -1;
  int failed = 0;
  for (struct dirent* entry; !failed && (entry = readdir(dir));) {
    if (is_dot(entry->d_name)) continue;
    char source[512], target[512], temporary[512];
    struct stat info, old;
    snprintf(source, sizeof source, "%s/%s", from, entry->d_name);
    snprintf(target, sizeof target, "%s/%s", to, entry->d_name);
    snprintf(temporary, sizeof temporary, "%s/.%s.new", to, entry->d_name);
    int exists = !lstat(target, &old);
    if (stat(source, &info)) failed = -1;
    else if (S_ISDIR(info.st_mode))
      failed = (exists && !S_ISDIR(old.st_mode) && remove_tree(target)) || swap_tree(source, target);
    else if (S_ISREG(info.st_mode))
      failed = (exists && S_ISDIR(old.st_mode) && remove_tree(target)) || copy_file(source, temporary) ||
               rename(temporary, target);
  }
  closedir(dir);
  if (failed || !(dir = opendir(to))) return -1;
  for (int removed = 1; removed && !failed;) {
    removed = 0;
    rewinddir(dir);
    for (struct dirent* entry; !failed && (entry = readdir(dir));) {
      char source[512], target[512];
      struct stat info;
      snprintf(source, sizeof source, "%s/%s", from, entry->d_name);
      if (is_dot(entry->d_name) || !lstat(source, &info)) continue;
      snprintf(target, sizeof target, "%s/%s", to, entry->d_name);
      failed = remove_tree(target);
      removed = 1;
    }
  }
  closedir(dir);
  return failed ? -1 : 0;
}

// ---- Report ----
//
// What app.db holds about the Games (dispLocation 138) and Media (188) tabs, for a bug report: the columns that
// tell the two apart, and no account data.

static const char* report_dir = "/data/lineup";

static void diag_error(Buffer* b, const char* message) {
  puts_(b, "{\"error\":");
  put_json_string(b, message);
  puts_(b, "}");
}

// Each result row as an object keyed by column name. Returns -1 when the query fails, with the error in `b`.
static int diag_query(Buffer* b, sqlite3* db, const char* sql) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) { diag_error(b, sqlite3_errmsg(db)); return -1; }
  puts_(b, "[");
  int step, row = 0;
  for (; (step = sqlite3_step(statement)) == SQLITE_ROW; ++row) {
    puts_(b, row ? ",{" : "{");
    for (int column = 0; column < sqlite3_column_count(statement); ++column) {
      if (column) puts_(b, ",");
      put_json_string(b, sqlite3_column_name(statement, column));
      puts_(b, ":");
      int type = sqlite3_column_type(statement, column);
      if (type == SQLITE_INTEGER) putf(b, "%lld", (long long)sqlite3_column_int64(statement, column));
      else if (type == SQLITE_NULL) puts_(b, "null");
      else put_json_string(b, (const char*)sqlite3_column_text(statement, column));
    }
    puts_(b, "}");
  }
  if (step != SQLITE_DONE) {
    puts_(b, row ? "," : "");
    diag_error(b, sqlite3_errmsg(db));
  }
  sqlite3_finalize(statement);
  puts_(b, "]");
  return step == SQLITE_DONE ? 0 : -1;
}

// Writes report_dir/report.json, its path in `path`. Returns the HTTP status; `message` explains a failure.
static int write_report(char* path, size_t path_size, char* message, size_t message_size) {
  sqlite3* db = open_db(0);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return 500; }
  Buffer b = {0};
  // A query that fails still leaves its error in the file, which is saved anyway and reported as incomplete.
  int failed = 0;
  char error[200] = "";
  puts_(&b, "{\"version\":\"" LINEUPD_VERSION "\",\"triggers\":");
  failed |= diag_query(&b, db, "SELECT name, tbl_name, CASE WHEN name GLOB 'lineup_pin_*' THEN sql LIKE '%" PIN_TABS
                       "%' END AS current FROM sqlite_master WHERE type = 'trigger' ORDER BY name");
  if (failed && !*error) snprintf(error, sizeof error, "%s", sqlite3_errmsg(db));
  puts_(&b, ",\"profiles\":[");
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT substr(name, 14) FROM sqlite_master WHERE type = 'table' AND name GLOB "
                         "'tbl_iconinfo_[0-9]*' ORDER BY name", -1, &statement, NULL) != SQLITE_OK) {
    failed = -1;
    if (!*error) snprintf(error, sizeof error, "%s", sqlite3_errmsg(db));
  } else {
    for (int first = 1; sqlite3_step(statement) == SQLITE_ROW;) {
      const char* user = (const char*)sqlite3_column_text(statement, 0);
      if (!user || !is_user_id(user)) continue;
      puts_(&b, first ? "{\"user\":" : ",{\"user\":");
      first = 0;
      put_json_string(&b, user);
      puts_(&b, ",\"apps\":");
      char sql[400];
      snprintf(sql, sizeof sql, "SELECT titleId, titleName, dispLocation, visible, lastAccessIndex, pinnedStatus, "
               "appDrmType, lastLaunchMethod, deeplinkUri, hubAppUri FROM tbl_iconinfo_%s WHERE dispLocation IN "
               "(138, 188) ORDER BY dispLocation, lastAccessIndex DESC", user);
      if (diag_query(&b, db, sql)) {
        failed = -1;
        if (!*error) snprintf(error, sizeof error, "%s", sqlite3_errmsg(db));
      }
      puts_(&b, "}");
    }
    sqlite3_finalize(statement);
  }
  puts_(&b, "],\"content\":");
  if (diag_query(&b, db, "SELECT titleId, titleName, dispLocation, viewCategory, categoryType, appType, "
                 "is3rdPartyVideoApp, appinfoContentType, contentStatus, installStatus, contentLocation, pprDeeplinkUri, "
                 "pprHubAppUri FROM tbl_contentinfo ORDER BY dispLocation, titleId")) {
    failed = -1;
    if (!*error) snprintf(error, sizeof error, "%s", sqlite3_errmsg(db));
  }
  puts_(&b, "}\n");
  sqlite3_close(db);
  make_dirs(report_dir);
  snprintf(path, path_size, "%s/report.json", report_dir);
  FILE* file = b.data ? fopen(path, "wb") : NULL;
  int unwritten = !file || fwrite(b.data, 1, b.size, file) != b.size;
  if (file && fclose(file)) unwritten = 1;
  free(b.data);
  if (unwritten) { snprintf(message, message_size, "Could not write %s (%s).", path, strerror(errno)); return 500; }
  if (failed) {
    snprintf(message, message_size, "Part of the database could not be read (%s); %s was saved without it.", error, path);
    return 500;
  }
  return 200;
}

// ---- TabShifter's leftovers ----
//
// TabShifter put a "TS" launcher tile, TBSH00001, into the home screen's databases and three folders of its own, and
// it stays after TabShifter is deleted. Removing it takes every row of that title and its concept (cid:local:TBSH00001)
// out of both databases, after a backup of each, in one transaction, then the folders. Its nsx_content_flag_* rows
// in tbl_info_<user> stay: the system writes rows of that name too, so TabShifter's cannot be told from them.
#define TABSHIFTER_TITLE "TBSH00001"
#define TABSHIFTER_CONCEPT "cid:local:" TABSHIFTER_TITLE

static const char* tabshifter_dirs[] = {"/user/app/" TABSHIFTER_TITLE, "/user/appmeta/" TABSHIFTER_TITLE,
                                              "/system_data/priv/appmeta/" TABSHIFTER_TITLE};

// Deletes the rows of `schema`.`table` whose `column` is `value`, when the table has that column. Returns the rows
// deleted, or -1.
static int delete_where(sqlite3* db, const char* schema, const char* table, const char* column, const char* value) {
  if (!has_column(db, schema, table, column)) return 0;
  char sql[200];
  snprintf(sql, sizeof sql, "DELETE FROM %s.\"%s\" WHERE \"%s\" = ?2", schema, table, column);
  return run_values(db, sql, 0, value, NULL);
}

// The rows of TabShifter's tile in both databases, or -1. appinfo.db is attached as info.
static int tabshifter_rows(sqlite3* db, char users[][16], int count) {
  static const char* const places[][3] = {
    {"main", "tbl_contentinfo", "titleId"}, {"main", "tbl_conceptmetadata", "localConceptId"},
    {"info", "tbl_appinfo", "titleId"}, {"info", "tbl_conceptinfo", "local_concept_id"},
  };
  int total = 0;
  for (int i = 0; i < 4; ++i) {
    if (!has_column(db, places[i][0], places[i][1], places[i][2])) continue;
    char sql[200];
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s.%s WHERE %s IN ('" TABSHIFTER_TITLE "', '" TABSHIFTER_CONCEPT
             "')", places[i][0], places[i][1], places[i][2]);
    sqlite3_stmt* statement;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(statement) == SQLITE_ROW) total += sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
  }
  for (int i = 0; i < count; ++i) {
    char table[64], sql[200];
    snprintf(table, sizeof table, "tbl_iconinfo_%s", users[i]);
    if (!has_column(db, "main", table, "titleId")) continue;
    snprintf(sql, sizeof sql, "SELECT 1 FROM %s WHERE titleId = '" TABSHIFTER_TITLE "'", table);
    int found = finds_row(db, sql, NULL);
    if (found < 0) return -1;
    total += found;
  }
  return total;
}

// Removes TabShifter's tile, after backing up both databases. `removed` gets the rows taken out, `folders` the folders
// that were there and went. Returns the HTTP status; `message` explains a failure.
static int remove_tabshifter(int* removed, int* folders, char* backup, size_t backup_size, char* message,
                             size_t message_size) {
  *removed = *folders = 0;
  sqlite3* db = open_db(1);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return 500; }
  int status = 500, began = 0, failed = 0;
  static char users[MAX_GAMES][16];
  int count = icon_users(db, users);
  if (count < 0) { snprintf(message, message_size, "Could not read the profiles (%s).", sqlite3_errmsg(db)); goto done; }
  if (attach_appinfo(db, message, message_size)) goto done;
  int rows = tabshifter_rows(db, users, count);
  if (rows < 0) { snprintf(message, message_size, "Could not read the databases (%s).", sqlite3_errmsg(db)); goto done; }
  if (rows) {
    if (check_and_backup(db, backup, backup_size, message, message_size) ||
        backup_appinfo(db, backup, message, message_size)) goto done;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
      snprintf(message, message_size, "The database is busy (%s). Try again.", sqlite3_errmsg(db));
      goto done;
    }
    began = 1;
    const char* const shared[][3] = {
      {"main", "tbl_contentinfo", "titleId"}, {"main", "tbl_conceptmetadata", "localConceptId"},
      {"info", "tbl_appinfo", "titleId"}, {"info", "tbl_conceptinfo", "local_concept_id"},
    };
    for (int i = 0; !failed && i < 4; ++i) {
      int title = delete_where(db, shared[i][0], shared[i][1], shared[i][2], TABSHIFTER_TITLE);
      int concept = title < 0 ? -1 : delete_where(db, shared[i][0], shared[i][1], shared[i][2], TABSHIFTER_CONCEPT);
      failed = title < 0 || concept < 0;
      if (!failed) *removed += title + concept;
    }
    for (int i = 0; !failed && i < count; ++i) {
      char icon[64], concept[64], group[64];
      snprintf(icon, sizeof icon, "tbl_iconinfo_%s", users[i]);
      snprintf(concept, sizeof concept, "tbl_concepticoninfo_%s", users[i]);
      snprintf(group, sizeof group, "tbl_group_%s", users[i]);
      const char* const places[][2] = {{icon, "titleId"}, {concept, "localConceptId"}, {concept, "primaryTitleId"},
                                       {group, "itemId"}};
      const char* const values[] = {TABSHIFTER_TITLE, TABSHIFTER_CONCEPT, TABSHIFTER_TITLE, TABSHIFTER_TITLE};
      for (int j = 0; !failed && j < 4; ++j) {
        int changed = delete_where(db, "main", places[j][0], places[j][1], values[j]);
        failed = changed < 0;
        if (!failed) *removed += changed;
      }
      // Lineup's own rows of it, a pin or a hiding, go too.
      if (!failed && has_object(db, "lineup_pin")) failed = forget_pin(db, users[i], TABSHIFTER_TITLE);
    }
    if (!failed && has_object(db, "lineup_hidden"))
      failed = delete_where(db, "main", "lineup_hidden", "titleId", TABSHIFTER_TITLE) < 0;
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
      snprintf(message, message_size, "The write failed (%s). Nothing changed.", sqlite3_errmsg(db));
      *removed = 0;
      goto done;
    }
    began = 0;
  }
  // The folders, once no row points at them.
  for (int i = 0; i < 3; ++i) {
    struct stat info;
    if (lstat(tabshifter_dirs[i], &info)) continue;
    if (remove_tree(tabshifter_dirs[i])) LOG("could not remove %s: %s", tabshifter_dirs[i], strerror(errno));
    else ++*folders;
  }
  status = 200;
done:
  if (began) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
  sqlite3_close(db);
  return status;
}

#ifndef LINEUPD_TEST

// ---- HTTP ----

static int send_all(int socket, const void* data, size_t size) {
  for (const char* p = data; size;) {
    ssize_t sent = send(socket, p, size, 0);
    if (sent <= 0) return -1;
    p += sent;
    size -= (size_t)sent;
  }
  return 0;
}

static const char* reason(int status) {
  switch (status) {
    case 200: return "OK"; case 400: return "Bad Request"; case 403: return "Forbidden";
    case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 409: return "Conflict";
    default: return "Internal Server Error";
  }
}

static void respond(int socket, int status, const char* type, const char* body, size_t size) {
  char header[256];
  int length = snprintf(header, sizeof header, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, reason(status), type, size);
  if (!send_all(socket, header, (size_t)length)) send_all(socket, body, size);
}

static void respond_json(int socket, int status, Buffer* body) {
  respond(socket, status, "application/json; charset=utf-8", body->data ? body->data : "{}", body->size);
}

static void respond_error(int socket, int status, const char* message) {
  Buffer body = {0};
  puts_(&body, "{\"error\":");
  put_json_string(&body, message);
  puts_(&body, "}");
  respond_json(socket, status, &body);
  free(body.data);
  LOG("%d %s", status, message);
}

static void get_profiles(int socket) {
  sqlite3* db = open_db(0);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  Buffer body = {0};
  puts_(&body, "{\"profiles\":[");
  DIR* dir = opendir(home_dir);
  int first = 1;
  for (struct dirent* entry; dir && (entry = readdir(dir));) {
    char* end;
    unsigned long id = strtoul(entry->d_name, &end, 16);
    if (strlen(entry->d_name) != 8 || *end) continue;
    char user[16], path[256], name[64] = "";
    snprintf(user, sizeof user, "%010lu", id);
    if (!table_exists(db, user)) continue;
    snprintf(path, sizeof path, "%s/%s/username.dat", home_dir, entry->d_name);
    FILE* file = fopen(path, "rb");
    if (file) {
      size_t size = fread(name, 1, sizeof name - 1, file);
      fclose(file);
      name[size] = 0;
      for (size_t i = strlen(name); i && isspace((unsigned char)name[i - 1]); --i) name[i - 1] = 0;
    }
    puts_(&body, first ? "{\"userId\":" : ",{\"userId\":");
    first = 0;
    put_json_string(&body, user);
    puts_(&body, ",\"hex\":");
    put_json_string(&body, entry->d_name);
    puts_(&body, ",\"name\":");
    put_json_string(&body, *name ? name : entry->d_name);
    char avatar[512];
    struct stat info;
    putf(&body, ",\"avatar\":%lld}", avatar_path(user, avatar, sizeof avatar) || stat(avatar, &info) ? 0
         : (long long)info.st_mtime);
  }
  if (dir) closedir(dir);
  puts_(&body, "]}");
  sqlite3_close(db);
  respond_json(socket, 200, &body);
  free(body.data);
}

// `media`: the Media tab instead of the Games tab.
static void get_row(int socket, const char* user, int media) {
  static Row rows[MAX_GAMES];
  static char names[MAX_GAMES][256];
  sqlite3* db = open_db(0);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  if (!table_exists(db, user)) { sqlite3_close(db); respond_error(socket, 404, "No such profile."); return; }
  int count = read_tab(db, user, media ? MEDIA_TAB : GAMES_TAB, rows, names, MAX_GAMES);
  static long long icons[MAX_GAMES];
  static int slots[MAX_GAMES], movable[MAX_GAMES];
  for (int i = 0; i < count; ++i) {
    char path[256];
    struct stat info;
    icon_path(db, rows[i].title, path, sizeof path);
    icons[i] = stat(path, &info) ? 0 : (long long)info.st_mtime;
    movable[i] = is_movable(db, rows[i].title) || !strcmp(rows[i].title, GALLERY_TITLE);
  }
  const char* pins = read_pins(db, user, rows, count < 0 ? 0 : count, slots);
  sqlite3_close(db);
  if (count < 0) { respond_error(socket, 500, "Could not read the row."); return; }
  Buffer body = {0};
  puts_(&body, "{\"user\":");
  put_json_string(&body, user);
  putf(&body, ",\"tab\":\"%s\",\"pins\":\"%s\",\"games\":[", media ? "media" : "games", pins);
  for (int i = 0; i < count; ++i) {
    puts_(&body, i ? ",{\"titleId\":" : "{\"titleId\":");
    put_json_string(&body, rows[i].title);
    puts_(&body, ",\"name\":");
    put_json_string(&body, names[i]);
    putf(&body, ",\"index\":%lld,\"icon\":%lld,\"movable\":%s", rows[i].index, icons[i], movable[i] ? "true" : "false");
    if (slots[i] < 0) puts_(&body, ",\"pinned\":null}");
    else putf(&body, ",\"pinned\":%d}", slots[i]);
  }
  puts_(&body, "]}");
  respond_json(socket, 200, &body);
  free(body.data);
}

static void get_hidden(int socket, const char* user) {
  sqlite3* db = open_db(0);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  if (!table_exists(db, user)) { sqlite3_close(db); respond_error(socket, 404, "No such profile."); return; }
  Buffer body = {0};
  puts_(&body, "{\"hidden\":[");
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT i.titleId, i.titleName FROM tbl_iconinfo_%s i JOIN lineup_hidden h ON h.titleId = "
           "i.titleId WHERE h.user = ? AND i.visible = 0 ORDER BY i.titleName", user);
  sqlite3_stmt* statement;
  if (has_object(db, "lineup_hidden") && sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK) {
    sqlite3_bind_text(statement, 1, user, -1, SQLITE_STATIC);
    for (int row = 0; sqlite3_step(statement) == SQLITE_ROW; ++row) {
      puts_(&body, row ? ",{\"titleId\":" : "{\"titleId\":");
      put_json_string(&body, (const char*)sqlite3_column_text(statement, 0));
      puts_(&body, ",\"name\":");
      put_json_string(&body, (const char*)sqlite3_column_text(statement, 1));
      puts_(&body, "}");
    }
    sqlite3_finalize(statement);
  }
  // The system's tiles Lineup manages on this profile: those showing on a tab (which can hide), and the PS Store, the
  // Media Gallery, App Library and Welcome whatever their state. tab is games, media or none (off both); shown, that
  // Lineup shows it.
  puts_(&body, "],\"system\":[");
  char tiles[1024];
  int content = has_column(db, "main", "tbl_contentinfo", "dispLocation");
  int kept = has_object_in(db, "main", "lineup_system");
  snprintf(tiles, sizeof tiles, "SELECT i.titleId, ifnull(i.titleName, i.titleId), i.visible, %1$s, %2$s FROM "
           "tbl_iconinfo_%3$s i WHERE i.titleId GLOB 'NPXS*' AND i.titleId <> '" GAME_LIBRARY_TITLE "' AND "
           "((i.visible = 1 AND (%1$s & 6) <> 0) OR i.titleId IN " LISTED_SYSTEM " OR i.titleId = '" WELCOME_TITLE "') "
           "ORDER BY i.titleName",
           content ? "ifnull((SELECT c.dispLocation FROM tbl_contentinfo c WHERE c.titleId = i.titleId), i.dispLocation)"
                   : "i.dispLocation",
           kept ? "EXISTS (SELECT 1 FROM lineup_system s WHERE s.titleId = i.titleId AND s.visible = 1)" : "0", user);
  if (sqlite3_prepare_v2(db, tiles, -1, &statement, NULL) == SQLITE_OK) {
    for (int row = 0; sqlite3_step(statement) == SQLITE_ROW; ++row) {
      int bit = tab_bit(sqlite3_column_int64(statement, 3));
      puts_(&body, row ? ",{\"titleId\":" : "{\"titleId\":");
      put_json_string(&body, (const char*)sqlite3_column_text(statement, 0));
      puts_(&body, ",\"name\":");
      put_json_string(&body, (const char*)sqlite3_column_text(statement, 1));
      putf(&body, ",\"tab\":\"%s\",\"visible\":%s,\"shown\":%s}",
           bit == BIT_GAMES ? "games" : bit == BIT_MEDIA ? "media" : "none",
           sqlite3_column_int(statement, 2) == 1 ? "true" : "false", sqlite3_column_int(statement, 4) ? "true" : "false");
    }
    sqlite3_finalize(statement);
  }
  sqlite3_close(db);
  puts_(&body, "]}");
  respond_json(socket, 200, &body);
  free(body.data);
}

static void post_tabshifter(int socket) {
  char backup[256] = "", message[300] = "";
  int removed = 0, folders = 0;
  int status = remove_tabshifter(&removed, &folders, backup, sizeof backup, message, sizeof message);
  if (status != 200) { respond_error(socket, status, message); return; }
  LOG("TabShifter's tile: %d rows and %d folders removed, backup %s", removed, folders, *backup ? backup : "none");
  Buffer reply = {0};
  putf(&reply, "{\"ok\":true,\"removed\":%d,\"folders\":%d,\"backup\":", removed, folders);
  put_json_string(&reply, backup);
  puts_(&reply, "}");
  respond_json(socket, 200, &reply);
  free(reply.data);
}

static void send_png(int socket, const char* path, const char* missing) {
  int file = open(path, O_RDONLY);
  struct stat info;
  if (file < 0 || fstat(file, &info)) {
    if (file >= 0) close(file);
    respond_error(socket, 404, missing);
    return;
  }
  char header[192];
  int length = snprintf(header, sizeof header, "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %lld\r\n"
                        "Connection: close\r\n\r\n", (long long)info.st_size);
  static char chunk[64 * 1024];
  if (!send_all(socket, header, (size_t)length))
    for (ssize_t count; (count = read(file, chunk, sizeof chunk)) > 0;)
      if (send_all(socket, chunk, (size_t)count)) break;
  close(file);
}

static void get_icon(int socket, const char* title) {
  if (!is_title_id(title)) { respond_error(socket, 400, "Invalid title ID."); return; }
  char path[256];
  sqlite3* db = open_db(0);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  icon_path(db, title, path, sizeof path);
  sqlite3_close(db);
  send_png(socket, path, "This game has no icon.");
}

static void get_avatar(int socket, const char* user) {
  if (!is_user_id(user)) { respond_error(socket, 400, "user must be 10 digits."); return; }
  char path[512];
  if (avatar_path(user, path, sizeof path)) { respond_error(socket, 404, "This profile has no picture."); return; }
  send_png(socket, path, "This profile has no picture.");
}

static void post_row(int socket, const char* user, int location, const char* body) {
  static Row posted[MAX_GAMES];
  int count = parse_order(body, posted, MAX_GAMES);
  if (count <= 0) { respond_error(socket, 400, "The posted order is malformed."); return; }
  char backup[256] = "", integrity[256] = "", message[300] = "";
  int status = save_order(user, location, posted, count, backup, sizeof backup, integrity, sizeof integrity, message,
                          sizeof message);
  if (status != 200) { respond_error(socket, status, message); return; }
  LOG("saved %d apps of tab %d for %s, backup %s, integrity %s", count, location, user, backup, integrity);
  Buffer reply = {0};
  puts_(&reply, "{\"ok\":true,\"backup\":");
  put_json_string(&reply, backup);
  puts_(&reply, ",\"integrity\":");
  put_json_string(&reply, integrity);
  puts_(&reply, "}");
  respond_json(socket, 200, &reply);
  free(reply.data);
}

static void post_unpin(int socket, const char* user) {
  char backup[256] = "", message[300] = "";
  int status = unpin(user, backup, sizeof backup, message, sizeof message);
  if (status != 200) { respond_error(socket, status, message); return; }
  LOG("pins off for %s, backup %s", user ? user : "every profile", backup);
  Buffer reply = {0};
  puts_(&reply, "{\"ok\":true,\"backup\":");
  put_json_string(&reply, backup);
  puts_(&reply, "}");
  respond_json(socket, 200, &reply);
  free(reply.data);
}

static void post_app(int socket, const char* user, const char* title, const char* action) {
  char backup[256] = "", message[300] = "";
  int hide = !strcmp(action, "hide"), show = !strcmp(action, "show"), media = !strcmp(action, "media");
  if (!hide && !show && !media && strcmp(action, "games")) { respond_error(socket, 400, "action must be hide, show, games or media."); return; }
  int welcome = !strcmp(title, WELCOME_TITLE);
  if (welcome && !hide && !show) { respond_error(socket, 400, "Welcome shows or hides; it does not move."); return; }
  int status = welcome ? show_welcome(show, backup, sizeof backup, message, sizeof message)
             : hide || show ? set_hidden(user, title, hide, backup, sizeof backup, message, sizeof message)
             : is_system_movable(title) ? move_system(title, media, backup, sizeof backup, message, sizeof message)
             : move_app(title, media ? MEDIA_TAB : GAMES_TAB, backup, sizeof backup, message, sizeof message);
  if (status != 200) { respond_error(socket, status, message); return; }
  LOG("%s %s for %s, backup %s", action, title, user, backup);
  Buffer reply = {0};
  puts_(&reply, "{\"ok\":true,\"backup\":");
  put_json_string(&reply, backup);
  puts_(&reply, "}");
  respond_json(socket, 200, &reply);
  free(reply.data);
}

static void post_report(int socket) {
  char path[256] = "", message[300] = "";
  int status = write_report(path, sizeof path, message, sizeof message);
  if (status != 200) { respond_error(socket, status, message); return; }
  Buffer reply = {0};
  puts_(&reply, "{\"ok\":true,\"path\":");
  put_json_string(&reply, path);
  puts_(&reply, "}");
  respond_json(socket, 200, &reply);
  free(reply.data);
}

// Returns the value of `name` in a query string, copied into `out`.
static const char* query_value(const char* query, const char* name, char* out, size_t size) {
  size_t length = strlen(name);
  for (const char* p = query; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL)
    if (!strncmp(p, name, length) && p[length] == '=') {
      const char* value = p + length + 1;
      size_t n = strcspn(value, "&");
      if (n >= size) n = size - 1;
      memcpy(out, value, n);
      out[n] = 0;
      return out;
    }
  return NULL;
}

// ---- Updates ----

typedef struct {
  uint32_t size, user_id, app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} LncAppParam;

int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int*);
int sceLncUtilGetAppId(const char* title_id);
int sceLncUtilLaunchApp(const char* title_id, const char** argv, LncAppParam* param);

typedef struct { char path[256]; int image; } Install;

static int is_image_name(const char* name) {
  size_t length = strlen(name);
  return !strncmp(name, "Lineup-", 7) && length > 13 && !strcmp(name + length - 6, ".exfat");
}

// Where ShadowMountPlus finds Lineup: a TITLE_ID folder or a Lineup-*.exfat image. Returns how many there are
// and keeps the first in `install`; an update needs exactly one.
static int find_install(Install* install) {
  char roots[18][32] = {"/data/homebrew", "/data/etaHEN/games"};
  for (int i = 0; i < 8; ++i) {
    snprintf(roots[2 + i], sizeof roots[0], "/mnt/usb%d/homebrew", i);
    snprintf(roots[10 + i], sizeof roots[0], "/mnt/ext%d/homebrew", i);
  }
  int count = 0;
  for (int r = 0; r < 18; ++r) {
    DIR* dir = opendir(roots[r]);
    for (struct dirent* entry; dir && (entry = readdir(dir));) {
      int image = is_image_name(entry->d_name);
      if (!image && strcmp(entry->d_name, TITLE_ID)) continue;
      char path[256];
      struct stat info;
      snprintf(path, sizeof path, "%s/%s", roots[r], entry->d_name);
      if (stat(path, &info) || (image ? !S_ISREG(info.st_mode) : !S_ISDIR(info.st_mode))) continue;
      if (!count++) { snprintf(install->path, sizeof install->path, "%s", path); install->image = image; }
    }
    if (dir) closedir(dir);
  }
  return count;
}

// What `name` (a directory in update_dir) holds for this kind of install: the TITLE_ID folder or the one
// Lineup-<version>.exfat. Returns its version, or -1 when there is none.
static long saved_source(int image, const char* name, char* out, size_t size) {
  char dir_path[256];
  snprintf(dir_path, sizeof dir_path, "%s/%s", update_dir, name);
  if (!image) {
    snprintf(out, size, "%s/%s", dir_path, TITLE_ID);
    return app_folder_version(out);
  }
  long version = -1;
  DIR* dir = opendir(dir_path);
  for (struct dirent* entry; dir && version < 0 && (entry = readdir(dir));) {
    if (!is_image_name(entry->d_name) || !is_file(dir_path, entry->d_name)) continue;
    char text[32];
    snprintf(text, sizeof text, "%.*s", (int)strlen(entry->d_name) - 13, entry->d_name + 7);
    snprintf(out, size, "%s/%s", dir_path, entry->d_name);
    version = version_number(text);
  }
  if (dir) closedir(dir);
  return version;
}

// ShadowMountPlus mounts an image at /mnt/shadowmnt/<image name>_<hash>.
// ponytail: matched by name; compare the mount's backing file if a renamed image is ever missed.
static int image_mounted(const char* image) {
  const char* name = strrchr(image, '/') + 1;
  char stem[128];
  snprintf(stem, sizeof stem, "%.*s", (int)strlen(name) - 6, name);
  struct statfs* mounts;
  int count = getmntinfo(&mounts, MNT_NOWAIT);
  for (int i = 0; i < count; ++i)
    if (strstr(mounts[i].f_mntonname, stem) || strstr(mounts[i].f_mntfromname, stem)) return 1;
  return 0;
}

static struct { int pending, rollback; Install install; char source[256]; } update;

static void get_update_install(int socket) {
  Install install;
  int count = find_install(&install);
  if (count != 1) {
    respond_error(socket, 409, count ? "There is more than one Lineup folder or image. Keep only one."
                                     : "Could not find the Lineup folder or image.");
    return;
  }
  char source[512];
  long previous = saved_source(install.image, "previous", source, sizeof source);
  Buffer reply = {0};
  puts_(&reply, install.image ? "{\"kind\":\"image\",\"path\":" : "{\"kind\":\"folder\",\"path\":");
  put_json_string(&reply, install.path);
  if (previous < 0) puts_(&reply, ",\"previous\":\"\"}");
  else putf(&reply, ",\"previous\":\"%ld.%ld.%ld\"}", previous / 1000000, previous / 1000 % 1000, previous % 1000);
  respond_json(socket, 200, &reply);
  free(reply.data);
}

// Checks everything it can while the app still runs, answers, and leaves the swap to run_update.
static void post_update_apply(int socket, const char* query) {
  char version[16];
  if (!query_value(query, "version", version, sizeof version)) { respond_error(socket, 400, "version is missing."); return; }
  int rollback = !strcmp(version, "previous");
  long number = rollback ? 0 : version_number(version);
  if (!rollback && number <= version_number(LINEUPD_VERSION)) {
    respond_error(socket, 409, "Only a newer version installs this way.");
    return;
  }
  Install install;
  int count = find_install(&install);
  if (count != 1) {
    respond_error(socket, 409, count ? "There is more than one Lineup folder or image. Keep only one."
                                     : "Could not find the Lineup folder or image.");
    return;
  }
  // Every path comes from here, never from the request: update_dir/<version>/... or update_dir/previous/...
  char source[512];
  long found;
  struct stat info, target;
  if (rollback) found = saved_source(install.image, "previous", source, sizeof source);
  else if (install.image) {
    snprintf(source, sizeof source, "%s/%s/Lineup-%s.exfat", update_dir, version, version);
    found = !stat(source, &info) && S_ISREG(info.st_mode) ? number : -1;
  } else {
    snprintf(source, sizeof source, "%s/%s/stage/%s", update_dir, version, TITLE_ID);
    found = app_folder_version(source);
  }
  if (found < 0 || (!rollback && found != number)) {
    respond_error(socket, 409, rollback ? "There is no previous version." : "The downloaded update is missing or incomplete.");
    return;
  }
  if (install.image) {
    // An image is moved, not copied: it must be on the drive the installed one is on.
    char folder[256];
    snprintf(folder, sizeof folder, "%.*s", (int)(strrchr(install.path, '/') - install.path), install.path);
    if (stat(source, &info) || stat(folder, &target) || info.st_dev != target.st_dev) {
      char message[600];
      snprintf(message, sizeof message, "The image is on another drive. Put %s in %s by hand.", source, folder);
      respond_error(socket, 409, message);
      return;
    }
  }
  if (!install.image) {
    // Backed up now, while the app runs and shows its wait, so the swap after it closes is short. A folder
    // install is copied in place later (never renamed: it is nullfs-mounted), so this copy is the way back.
    char staged[256], saved[512];
    snprintf(staged, sizeof staged, "%s/previous.new", update_dir);
    snprintf(saved, sizeof saved, "%s/%s", staged, strrchr(install.path, '/') + 1);
    int failed = remove_tree(staged);
    if (!failed) { make_dirs(staged); failed = copy_tree(install.path, saved); }
    if (failed) {
      char message[600];
      snprintf(message, sizeof message, "Could not back up the installed version (%s). Nothing changed.", strerror(errno));
      remove_tree(staged);
      respond_error(socket, 500, message);
      return;
    }
    sync();
  }
  update.pending = 1;
  update.rollback = rollback;
  update.install = install;
  snprintf(update.source, sizeof update.source, "%s", source);
  LOG("update to %s from %s, %s", version, source, install.path);
  // Tells the app, when it opens, to wait until run_update is done.
  char marker[256];
  snprintf(marker, sizeof marker, "%s/applying", update_dir);
  FILE* file = fopen(marker, "w");
  if (file) { fputs(version, file); fclose(file); }
  else LOG("could not write %s: %s", marker, strerror(errno));
  Buffer reply = {0};
  puts_(&reply, "{\"ok\":true}");
  respond_json(socket, 200, &reply);
  free(reply.data);
}

// Leaves `message` in update_dir/error, for the app to show when it opens again.
static void update_failed(const char* format, ...) {
  char message[600], path[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof message, format, args);
  va_end(args);
  LOG("update failed: %s", message);
  snprintf(path, sizeof path, "%s/error", update_dir);
  FILE* file = fopen(path, "w");
  if (file) { fputs(message, file); fclose(file); }
}

// Swaps the installed files for update.source. A folder install was copied to update_dir/previous.new when the
// update was asked for; once swapped, that copy replaces previous/ (which holds the source when rolling back).
// An image goes to previous/ as it is moved; a rollback leaves previous/ alone, as its source image is in it.
static void replace_files(void) {
  char previous[256], staged[256], saved[512];
  snprintf(previous, sizeof previous, "%s/previous", update_dir);
  snprintf(staged, sizeof staged, "%s/previous.new", update_dir);
  const char* source = update.source;
  const char* installed = update.install.path;
  int image = update.install.image, failed = image && !update.rollback ? remove_tree(previous) : 0;
  if (image) make_dirs(previous);
  snprintf(saved, sizeof saved, "%s/%s", previous, strrchr(installed, '/') + 1);
  if (failed) update_failed("Could not clear %s (%s). Nothing changed.", previous, strerror(errno));
  else if (image) {
    // ShadowMountPlus lets the image go when the app closes, unless persistent_image_mounts is on.
    for (int i = 0; i < 20 && image_mounted(installed); ++i) usleep(500 * 1000);
    char target[512];
    snprintf(target, sizeof target, "%.*s/%s", (int)(strrchr(installed, '/') - installed), installed,
             strrchr(source, '/') + 1);
    if ((failed = image_mounted(installed)))
      update_failed("The image is still mounted (persistent_image_mounts?). Nothing changed: put %s in place of %s by hand.",
                    source, installed);
    else if ((failed = rename(installed, saved)))
      update_failed("Could not move %s aside (%s). Nothing changed.", installed, strerror(errno));
    else if ((failed = rename(source, target))) {
      update_failed("Could not move the new image into place (%s). Nothing changed.", strerror(errno));
      rename(saved, installed);
    } else {
      // ShadowMountPlus looks every 15 s and takes a new source once it has been still for 10 s.
      sleep(25);
    }
  } else if ((failed = swap_tree(source, installed))) {
    snprintf(saved, sizeof saved, "%s/%s", staged, strrchr(installed, '/') + 1);
    int restored = !swap_tree(saved, installed);
    update_failed("Could not replace the files (%s). %s", strerror(errno),
                  restored ? "The installed version was put back." : "Copy the app folder again by FTP.");
    remove_tree(staged);
  } else if (remove_tree(previous) || rename(staged, previous)) {
    LOG("could not keep the backup as %s: %s", previous, strerror(errno));
  }
  sync();
}

static void clear_applying(void) {
  char marker[256];
  snprintf(marker, sizeof marker, "%s/applying", update_dir);
  unlink(marker);
}

// Waits for the app to close, then swaps the files. Returns 1 when lineupd should hand over to the installed
// lineupd.elf; 0 when nothing changed. The caller ends with finish_update.
static int run_update(void) {
  update.pending = 0;
  char error[256], staged[256];
  snprintf(error, sizeof error, "%s/error", update_dir);
  unlink(error);
  for (int waited = 0; sceLncUtilGetAppId(TITLE_ID) >= 0; waited += 100) {
    if (waited >= 30000) {
      update_failed("Lineup did not close within 30 seconds. Nothing changed.");
      snprintf(staged, sizeof staged, "%s/previous.new", update_dir);
      remove_tree(staged);
      clear_applying();
      return 0;
    }
    usleep(100 * 1000);
  }
  usleep(500 * 1000);
  replace_files();
  return 1;
}

// Sends the installed lineupd.elf to the ELF loader, which starts it. 0 once the whole file is out.
static int hand_over(void) {
  static char chunk[64 * 1024];
  char path[300] = "/system_ex/app/" TITLE_ID "/lineupd.elf";
  int in = open(path, O_RDONLY);
  if (in < 0 && !update.install.image) {
    snprintf(path, sizeof path, "%s/lineupd.elf", update.install.path);
    in = open(path, O_RDONLY);
  }
  if (in < 0) { LOG("hand over: could not open %s: %s", path, strerror(errno)); return -1; }
  int out = socket(AF_INET, SOCK_STREAM, 0), failed = out < 0;
  struct sockaddr_in loader = {0};
  loader.sin_family = AF_INET;
  loader.sin_port = htons(9021);
  loader.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (!failed) failed = connect(out, (struct sockaddr*)&loader, sizeof loader);
  for (ssize_t count; !failed && (count = read(in, chunk, sizeof chunk)) != 0;)
    failed = count < 0 || send_all(out, chunk, (size_t)count);
  if (failed) LOG("hand over: could not send %s to the loader: %s", path, strerror(errno));
  close(in);
  if (out >= 0) close(out);
  return failed ? -1 : 0;
}

// What relauncher.c does, after the swap: open the app again for the foreground user. The marker goes first, so
// an app the user reopened early (it waits on the marker, then relaunches itself) is not launched twice.
static void finish_update(void) {
  int open_now = sceLncUtilGetAppId(TITLE_ID) >= 0;
  clear_applying();
  if (open_now) { LOG("%s is already open; it relaunches itself", TITLE_ID); return; }
  sceUserServiceInitialize(NULL);
  LncAppParam param = {sizeof param, 0, 0, 0, 0};
  sceUserServiceGetForegroundUser((int*)&param.user_id);
  LOG("launch %s: %d", TITLE_ID, sceLncUtilLaunchApp(TITLE_ID, NULL, &param));
}

#ifdef LINEUPD_DIAG

// ---- Phase 0 diagnostics ----

// A directory as [{"name","size","mtime","dir"}], `depth` levels deep, at most 200 entries a level.
static void diag_list(Buffer* b, const char* path, int depth) {
  DIR* dir = opendir(path);
  if (!dir) { diag_error(b, strerror(errno)); return; }
  puts_(b, "[");
  int count = 0;
  for (struct dirent* entry; (entry = readdir(dir)) && count < 200;) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    char child[512];
    struct stat info;
    snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
    int ok = !stat(child, &info);
    puts_(b, count++ ? ",{\"name\":" : "{\"name\":");
    put_json_string(b, entry->d_name);
    if (ok) putf(b, ",\"size\":%lld,\"mtime\":%lld", (long long)info.st_size, (long long)info.st_mtime);
    if (ok && S_ISDIR(info.st_mode)) {
      puts_(b, ",\"dir\":");
      if (depth > 1) diag_list(b, child, depth - 1); else puts_(b, "true");
    }
    puts_(b, "}");
  }
  closedir(dir);
  puts_(b, "]");
}

// Lineup sources ShadowMountPlus could pick up under `root`: a PPSA99077 folder or a Lineup-*.exfat image.
static void diag_candidates(Buffer* b, const char* root, int* first) {
  DIR* dir = opendir(root);
  if (!dir) return;
  for (struct dirent* entry; (entry = readdir(dir));) {
    size_t length = strlen(entry->d_name);
    if (strcmp(entry->d_name, "PPSA99077") &&
        (strncmp(entry->d_name, "Lineup-", 7) || length < 6 || strcmp(entry->d_name + length - 6, ".exfat"))) continue;
    char path[512];
    struct stat info;
    snprintf(path, sizeof path, "%s/%s", root, entry->d_name);
    puts_(b, *first ? "{\"path\":" : ",{\"path\":");
    *first = 0;
    put_json_string(b, path);
    if (!stat(path, &info)) putf(b, ",\"size\":%lld,\"mtime\":%lld,\"dir\":%s", (long long)info.st_size,
                                 (long long)info.st_mtime, S_ISDIR(info.st_mode) ? "true" : "false");
    puts_(b, "}");
  }
  closedir(dir);
}

// Strings in a system SQLite module that tell its version and build options; empty when it is encrypted.
static void diag_sqlite_module(Buffer* b, const char* path) {
  int file = open(path, O_RDONLY);
  struct stat info;
  if (file < 0 || fstat(file, &info) || info.st_size <= 0 || info.st_size > 32 * 1024 * 1024) {
    if (file >= 0) close(file);
    diag_error(b, file < 0 ? strerror(errno) : "unexpected size");
    return;
  }
  unsigned char* data = malloc((size_t)info.st_size);
  ssize_t size = data ? read(file, data, (size_t)info.st_size) : -1;
  close(file);
  if (size < 4) { free(data); diag_error(b, "read failed"); return; }
  putf(b, "{\"size\":%lld,\"magic\":\"%02x%02x%02x%02x\",\"strings\":[", (long long)size, data[0], data[1], data[2], data[3]);
  int found = 0;
  for (ssize_t start = 0, i = 0; i <= size && found < 150; ++i) {
    if (i < size && data[i] >= 0x20 && data[i] < 0x7f) continue;
    ssize_t length = i - start;
    char text[128];
    if (length >= 4 && length < (ssize_t)sizeof text) {
      memcpy(text, data + start, (size_t)length);
      text[length] = 0;
      int version = text[0] == '3' && text[1] == '.' && strspn(text, "0123456789.") == (size_t)length;
      int source_id = length > 50 && !strncmp(text, "20", 2) && text[4] == '-' && text[10] == ' ';
      if (version || source_id || strstr(text, "OMIT_") || strstr(text, "ENABLE_") || strstr(text, "THREADSAFE")) {
        puts_(b, found++ ? "," : "");
        put_json_string(b, text);
      }
    }
    start = i + 1;
  }
  puts_(b, "]}");
  free(data);
}

static void get_diag(int socket) {
  Buffer b = {0};
  puts_(&b, "{\"version\":\"" LINEUPD_VERSION "-diag\",\"profileCache\":");
  diag_list(&b, profile_cache_dir, 2);
  puts_(&b, ",\"homes\":");
  diag_list(&b, home_dir, 2);

  puts_(&b, ",\"mounts\":");
  struct statfs* mounts;
  int count = getmntinfo(&mounts, MNT_NOWAIT);
  if (count <= 0) diag_error(&b, strerror(errno));
  else {
    puts_(&b, "[");
    for (int i = 0; i < count; ++i) {
      puts_(&b, i ? ",{\"from\":" : "{\"from\":");
      put_json_string(&b, mounts[i].f_mntfromname);
      puts_(&b, ",\"on\":");
      put_json_string(&b, mounts[i].f_mntonname);
      puts_(&b, ",\"type\":");
      put_json_string(&b, mounts[i].f_fstypename);
      putf(&b, ",\"flags\":%llu}", (unsigned long long)mounts[i].f_flags);
    }
    puts_(&b, "]");
  }

  puts_(&b, ",\"candidates\":[");
  int first = 1;
  diag_candidates(&b, "/data/homebrew", &first);
  diag_candidates(&b, "/data/etaHEN/games", &first);
  for (int i = 0; i < 8; ++i) {
    char root[64];
    snprintf(root, sizeof root, "/mnt/usb%d/homebrew", i);
    diag_candidates(&b, root, &first);
    snprintf(root, sizeof root, "/mnt/ext%d/homebrew", i);
    diag_candidates(&b, root, &first);
  }
  puts_(&b, "],\"shadowmnt\":");
  diag_list(&b, "/mnt/shadowmnt", 1);

  // The database header records the SQLite version that last wrote it: the home screen's own SQLite.
  puts_(&b, ",\"appDb\":");
  unsigned char header[100];
  int file = open(db_path, O_RDONLY);
  if (file < 0 || read(file, header, sizeof header) != (ssize_t)sizeof header) diag_error(&b, strerror(errno));
  else {
    unsigned writer = (unsigned)header[96] << 24 | header[97] << 16 | header[98] << 8 | header[99];
    unsigned valid = (unsigned)header[92] << 24 | header[93] << 16 | header[94] << 8 | header[95];
    putf(&b, "{\"lastWriterSqlite\":%u,\"versionValidFor\":%u,\"writeFormat\":%d,\"readFormat\":%d,\"schema\":",
         writer, valid, header[18], header[19]);
    sqlite3* db = open_db(0);
    if (!db) diag_error(&b, "open failed");
    else {
      diag_query(&b, db, "SELECT type, name, tbl_name, sql FROM sqlite_master WHERE type IN ('trigger','view') OR name LIKE 'lineup%'");
      puts_(&b, ",\"journalMode\":");
      diag_query(&b, db, "PRAGMA journal_mode");
      sqlite3_close(db);
    }
    puts_(&b, "}");
  }
  if (file >= 0) close(file);

  puts_(&b, ",\"sqliteModules\":{");
  const char* lib_dirs[] = {"/system/common/lib", "/system/priv/lib", "/system_ex/common_ex/lib", "/system/vsh"};
  first = 1;
  for (size_t d = 0; d < sizeof lib_dirs / sizeof *lib_dirs; ++d) {
    DIR* dir = opendir(lib_dirs[d]);
    for (struct dirent* entry; dir && (entry = readdir(dir));) {
      if (!strcasestr(entry->d_name, "sqlite")) continue;
      char path[512];
      snprintf(path, sizeof path, "%s/%s", lib_dirs[d], entry->d_name);
      if (!first) puts_(&b, ",");
      first = 0;
      put_json_string(&b, path);
      puts_(&b, ":");
      diag_sqlite_module(&b, path);
    }
    if (dir) closedir(dir);
  }
  puts_(&b, "}}");
  respond_json(socket, 200, &b);
  free(b.data);
}

// Log-only triggers: they record what the system writes to the row and change nothing else.
static void post_trace(int socket, const char* user) {
  sqlite3* db = open_db(1);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  char backup[256], message[300], sql[1600];
  if (!table_exists(db, user)) { sqlite3_close(db); respond_error(socket, 404, "No such profile."); return; }
  if (check_and_backup(db, backup, sizeof backup, message, sizeof message)) {
    sqlite3_close(db);
    respond_error(socket, 500, message);
    return;
  }
  snprintf(sql, sizeof sql,
           "BEGIN IMMEDIATE;"
           "CREATE TABLE IF NOT EXISTS lineup_trace (op TEXT, titleId TEXT, beforeIndex INTEGER, afterIndex INTEGER, at INTEGER);"
           "CREATE TRIGGER lineup_trace_u_%1$s AFTER UPDATE ON tbl_iconinfo_%1$s BEGIN INSERT INTO lineup_trace "
           "VALUES ('U', NEW.titleId, OLD.lastAccessIndex, NEW.lastAccessIndex, strftime('%%s','now')); END;"
           "CREATE TRIGGER lineup_trace_i_%1$s AFTER INSERT ON tbl_iconinfo_%1$s BEGIN INSERT INTO lineup_trace "
           "VALUES ('I', NEW.titleId, NULL, NEW.lastAccessIndex, strftime('%%s','now')); END;"
           "CREATE TRIGGER lineup_trace_d_%1$s AFTER DELETE ON tbl_iconinfo_%1$s BEGIN INSERT INTO lineup_trace "
           "VALUES ('D', OLD.titleId, OLD.lastAccessIndex, NULL, strftime('%%s','now')); END;"
           "COMMIT;", user);
  char* error = NULL;
  if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
    snprintf(message, sizeof message, "Could not install the trace (%s). Nothing changed.", error ? error : "?");
    sqlite3_free(error);
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    respond_error(socket, 409, message);
    return;
  }
  sqlite3_close(db);
  LOG("trace on for %s, backup %s", user, backup);
  Buffer reply = {0};
  puts_(&reply, "{\"ok\":true,\"backup\":");
  put_json_string(&reply, backup);
  puts_(&reply, "}");
  respond_json(socket, 200, &reply);
  free(reply.data);
}

static void get_trace(int socket) {
  sqlite3* db = open_db(0);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  Buffer b = {0};
  puts_(&b, "{\"triggers\":");
  diag_query(&b, db, "SELECT name, sql FROM sqlite_master WHERE type = 'trigger' AND name LIKE 'lineup_trace_%'");
  puts_(&b, ",\"rows\":");
  diag_query(&b, db, "SELECT rowid AS n, op, titleId, beforeIndex, afterIndex, at FROM lineup_trace ORDER BY rowid DESC LIMIT 500");
  puts_(&b, "}");
  sqlite3_close(db);
  respond_json(socket, 200, &b);
  free(b.data);
}

static void delete_trace(int socket) {
  sqlite3* db = open_db(1);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  static char names[64][64];
  int count = 0;
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'trigger' AND name LIKE 'lineup_trace_%'",
                         -1, &statement, NULL) == SQLITE_OK) {
    while (count < 64 && sqlite3_step(statement) == SQLITE_ROW)
      snprintf(names[count++], sizeof names[0], "%s", (const char*)sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
  }
  int failed = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK;
  for (int i = 0; !failed && i < count; ++i) {
    char sql[128];
    snprintf(sql, sizeof sql, "DROP TRIGGER \"%s\"", names[i]);
    failed = sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK;
  }
  if (!failed) failed = sqlite3_exec(db, "DROP TABLE IF EXISTS lineup_trace; COMMIT", NULL, NULL, NULL) != SQLITE_OK;
  if (failed) {
    char message[300];
    snprintf(message, sizeof message, "Could not remove the trace (%s).", sqlite3_errmsg(db));
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(db);
    respond_error(socket, 500, message);
    return;
  }
  sqlite3_close(db);
  LOG("trace off, %d triggers dropped", count);
  Buffer reply = {0};
  putf(&reply, "{\"ok\":true,\"dropped\":%d}", count);
  respond_json(socket, 200, &reply);
  free(reply.data);
}

#endif

// Handles one request. Returns 1 when the service should exit.
static int handle(int socket) {
  static char request[MAX_HEADER + MAX_BODY + 1];
  size_t size = 0;
  char* body = NULL;
  while (!body && size < MAX_HEADER) {
    ssize_t count = recv(socket, request + size, MAX_HEADER - size, 0);
    if (count <= 0) return 0;
    size += (size_t)count;
    request[size] = 0;
    char* end = strstr(request, "\r\n\r\n");
    if (end) body = end + 4;
  }
  if (!body) { respond_error(socket, 400, "Request too large."); return 0; }
  body[-2] = 0;  // Headers end at the first CRLF of the blank line.

  char method[8], target[512];
  if (sscanf(request, "%7s %511s", method, target) != 2) { respond_error(socket, 400, "Malformed request."); return 0; }
  size_t content_length = 0;
  int authorized = 0;
  for (char* line = strstr(request, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
    if (!strncasecmp(line + 2, "Content-Length:", 15)) content_length = strtoul(line + 17, NULL, 10);
    if (!strncasecmp(line + 2, "X-Lineup:", 9) && atoi(line + 11) == 1) authorized = 1;
  }
  if (content_length > MAX_BODY) { respond_error(socket, 400, "Body too large."); return 0; }
  size_t have = size - (size_t)(body - request);
  while (have < content_length) {
    ssize_t count = recv(socket, body + have, content_length - have, 0);
    if (count <= 0) return 0;
    have += (size_t)count;
  }
  body[content_length] = 0;

  char* query = strchr(target, '?');
  if (query) *query++ = 0;
  char user[16];
  int get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
  LOG("%s %s", method, target);

  if (get && !strcmp(target, "/version")) {
    Buffer reply = {0};
    puts_(&reply, "{\"version\":\"" LINEUPD_VERSION "\",\"build\":\"" LINEUPD_BUILD "\"}");
    respond_json(socket, 200, &reply);
    free(reply.data);
  } else if (get && !strcmp(target, "/profiles")) {
    get_profiles(socket);
  } else if (!strcmp(target, "/row") && (get || post)) {
    if (!query_value(query, "user", user, sizeof user) || !is_user_id(user))
      respond_error(socket, 400, "user must be 10 digits.");
    else {
      char tab[8] = "games";
      query_value(query, "tab", tab, sizeof tab);
      if (strcmp(tab, "games") && strcmp(tab, "media")) respond_error(socket, 400, "tab must be games or media.");
      else if (get) get_row(socket, user, !strcmp(tab, "media"));
      else if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
      else post_row(socket, user, !strcmp(tab, "media") ? MEDIA_TAB : GAMES_TAB, body);
    }
  } else if (post && !strncmp(target, "/pins/", 6)) {
    if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
    else if (!strcmp(target, "/pins/uninstall")) post_unpin(socket, NULL);
    else if (strcmp(target, "/pins/off")) respond_error(socket, 404, "Unknown route.");
    else if (!query_value(query, "user", user, sizeof user) || !is_user_id(user)) respond_error(socket, 400, "user must be 10 digits.");
    else post_unpin(socket, user);
  } else if (get && !strcmp(target, "/hidden")) {
    if (!query_value(query, "user", user, sizeof user) || !is_user_id(user)) respond_error(socket, 400, "user must be 10 digits.");
    else get_hidden(socket, user);
  } else if (post && !strcmp(target, "/app")) {
    char title[16] = "", action[8] = "";
    query_value(query, "title", title, sizeof title);
    query_value(query, "action", action, sizeof action);
    if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
    else if (!query_value(query, "user", user, sizeof user) || !is_user_id(user)) respond_error(socket, 400, "user must be 10 digits.");
    else if (!is_title_id(title)) respond_error(socket, 400, "Invalid title ID.");
    else post_app(socket, user, title, action);
  } else if (post && !strcmp(target, "/report")) {
    if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
    else post_report(socket);
  } else if (post && !strcmp(target, "/tabshifter/remove")) {
    if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
    else post_tabshifter(socket);
  } else if (get && !strncmp(target, "/icon/", 6)) {
    get_icon(socket, target + 6);
  } else if (get && !strncmp(target, "/avatar/", 8)) {
    get_avatar(socket, target + 8);
  } else if (!strncmp(target, "/update/", 8)) {
    if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
    else if (get && !strcmp(target, "/update/install")) get_update_install(socket);
    else if (post && !strcmp(target, "/update/apply")) post_update_apply(socket, query);
    else respond_error(socket, 404, "Unknown route.");
#ifdef LINEUPD_DIAG
  } else if (get && !strcmp(target, "/diag")) {
    get_diag(socket);
  } else if (!strcmp(target, "/diag/trace")) {
    if (get) get_trace(socket);
    else if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
    else if (!strcmp(method, "DELETE")) delete_trace(socket);
    else if (!post) respond_error(socket, 405, "Use GET, POST or DELETE.");
    else if (!query_value(query, "user", user, sizeof user) || !is_user_id(user)) respond_error(socket, 400, "user must be 10 digits.");
    else post_trace(socket, user);
#endif
  } else if (post && !strcmp(target, "/quit")) {
    if (!authorized) { respond_error(socket, 403, "Missing the X-Lineup header."); return 0; }
    Buffer reply = {0};
    puts_(&reply, "{\"ok\":true}");
    respond_json(socket, 200, &reply);
    free(reply.data);
    return 1;
  } else {
    respond_error(socket, 404, "Unknown route.");
  }
  return 0;
}

// The listening socket on PORT, or -1.
static int listen_on_port(void) {
  int server = socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_port = htons(PORT);
#ifdef LINEUPD_LAN
  address.sin_addr.s_addr = htonl(INADDR_ANY);
#else
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif
  if (server < 0 || bind(server, (struct sockaddr*)&address, sizeof address) || listen(server, 8)) {
    LOG("listen on %d failed: %s; another lineupd is probably running", PORT, strerror(errno));
    if (server >= 0) close(server);
    return -1;
  }
  LOG("version %s listening on %d", LINEUPD_VERSION, PORT);
  return server;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  // The loader hands over its connection as stdio; let it go so the service outlives the sender.
  int null = open("/dev/null", O_RDWR);
  if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }

  int server = listen_on_port();
  if (server < 0) return 0;
  for (int quit = 0, triggers_checked = 0; !quit;) {
    int client = accept(server, NULL, NULL);
    if (client < 0) { if (errno == EINTR) continue; break; }
    struct timeval timeout = {5, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    quit = handle(client);
    close(client);
    // Once, after the first answer: the app waits only a few seconds for the service to answer at all, and a busy
    // database could hold the backup that long.
    if (!quit && !triggers_checked) {
      triggers_checked = 1;
      // Both databases as they were, the first time this lineupd runs, before any of its checks writes.
      backup_originals();
      update_pin_triggers();
      restore_hidden();
      restore_moved();
      release_unmovable();
      restore_system();
    }
    // After the answer is out: the app closes once it has it.
    if (update.pending && run_update()) {
      // The port is closed before the new lineupd starts, and the app opens after it is out, so the app finds
      // the new version at once. If the loader cannot be reached, keep serving.
      close(server);
      int handed = !hand_over();
      if (handed) usleep(300 * 1000);
      else server = listen_on_port();
      finish_update();
      if (handed || server < 0) { server = -1; break; }
    }
  }
  close(server);
  LOG("exit");
  return 0;
}

#else

static long long given;  // the largest index the simulated system gave

static long long scalar(const char* sql) {
  sqlite3* db = open_db(0);
  sqlite3_stmt* statement;
  assert(db && sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW);
  long long value = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(db);
  return value;
}

// Runs `format` (a statement, %1$s the user and %2$s a title) as the system would, outside Lineup.
static void run(const char* format, const char* user, const char* title) {
  static char sql[4096];
  assert(snprintf(sql, sizeof sql, format, user, title) < (int)sizeof sql);
  sqlite3* db = open_db(1);
  assert(db && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_close(db);
}

// What the console does when a game opens: two raises to its counter plus one, a few seconds apart, between writes
// that leave the index alone.
static void open_game(const char* user, const char* title) {
  for (int i = 0; i < 2; ++i) {
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT MAX(lastAccessIndex) FROM tbl_iconinfo_%s", user);
    given = scalar(sql) + 1;
    snprintf(sql, sizeof sql, "UPDATE tbl_iconinfo_%%1$s SET lastAccessIndex = %lld WHERE titleId = '%%2$s'", given);
    run(sql, user, title);
    run("UPDATE tbl_iconinfo_%1$s SET titleName = titleName WHERE titleId = '%2$s'", user, title);
  }
}

// Checks tab `location` against `expected`, the apps' last digits from first to last, and that no index went past
// the largest the system gave.
static void expect_tab(const char* user, int location, const char* expected) {
  static Row rows[MAX_GAMES];
  sqlite3* db = open_db(0);
  int count = read_tab(db, user, location, rows, NULL, MAX_GAMES);
  sqlite3_close(db);
  char got[MAX_GAMES + 1];
  for (int i = 0; i < count; ++i) got[i] = rows[i].title[8];
  got[count] = 0;
  if (strcmp(got, expected)) { fprintf(stderr, "row %s, expected %s\n", got, expected); exit(1); }
  for (int i = 1; i < count; ++i) assert(rows[i].index < rows[i - 1].index);
  assert(rows[0].index <= given);
}

static void expect_row(const char* user, const char* expected) { expect_tab(user, GAMES_TAB, expected); }

// Saves tab `location` in `order` (last digits), pinning the apps in `pins` at their places.
static void save_tab(const char* user, int location, const char* order, const char* pins) {
  static Row current[MAX_GAMES], posted[MAX_GAMES];
  sqlite3* db = open_db(0);
  int count = read_tab(db, user, location, current, NULL, MAX_GAMES);
  sqlite3_close(db);
  assert(count == (int)strlen(order));
  for (int i = 0; i < count; ++i)
    for (int j = 0; j < count; ++j)
      if (current[j].title[8] == order[i]) { posted[i] = current[j]; posted[i].pinned = strchr(pins, order[i]) != NULL; }
  char backup[256], integrity[256], message[300];
  int status = save_order(user, location, posted, count, backup, sizeof backup, integrity, sizeof integrity, message,
                          sizeof message);
  if (status != 200) { fprintf(stderr, "%d %s\n", status, message); exit(1); }
}

static void save_digits(const char* user, const char* order, const char* pins) { save_tab(user, GAMES_TAB, order, pins); }

static const char* tab_pins_of(const char* user, int location, int* slots) {
  static Row rows[MAX_GAMES];
  sqlite3* db = open_db(0);
  int count = read_tab(db, user, location, rows, NULL, MAX_GAMES);
  const char* state = read_pins(db, user, rows, count, slots);
  sqlite3_close(db);
  return state;
}

static const char* pins_of(const char* user, int* slots) { return tab_pins_of(user, GAMES_TAB, slots); }

static int has_object_named(const char* name) {
  sqlite3* db = open_db(0);
  int exists = has_object(db, name);
  sqlite3_close(db);
  return exists;
}

// Runs `sql` on appinfo.db, as the system would.
static void run_info(const char* sql) {
  sqlite3* info;
  assert(sqlite3_open(appinfo_path, &info) == SQLITE_OK && sqlite3_exec(info, sql, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_close(info);
}

// `title`'s values in appinfo.db's tbl_appinfo, in order and joined by |; given a LIKE pattern instead, the names of
// appinfo.db's objects that match it.
static const char* info_values(const char* title) {
  static char text[512];
  sqlite3* info;
  sqlite3_stmt* statement;
  assert(sqlite3_open(appinfo_path, &info) == SQLITE_OK);
  assert(sqlite3_prepare_v2(info, strchr(title, '%') ? "SELECT group_concat(name, '|') FROM sqlite_master WHERE name LIKE ?"
                            : "SELECT group_concat(val, '|') FROM (SELECT val FROM tbl_appinfo WHERE titleId = ? ORDER BY "
                            "rowid)", -1, &statement, NULL) == SQLITE_OK);
  sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
  assert(sqlite3_step(statement) == SQLITE_ROW);
  const char* value = (const char*)sqlite3_column_text(statement, 0);
  snprintf(text, sizeof text, "%s", value ? value : "");
  sqlite3_finalize(statement);
  sqlite3_close(info);
  return text;
}

static int show_all_of_test(void) {
  sqlite3* db = open_db(1);
  int result = show_all(db);
  sqlite3_close(db);
  return result;
}

// Pinned games on the synthetic database: games 1 to 5 of user 1, in that order.
static void test_pins(void) {
  const char* one = "0000000001";
  int slots[MAX_GAMES];
  given = scalar("SELECT MAX(lastAccessIndex) FROM tbl_iconinfo_0000000001");
  // The Media tab holds the one app at 188.
  {
    static Row media[MAX_GAMES];
    sqlite3* db = open_db(0);
    assert(read_tab(db, one, MEDIA_TAB, media, NULL, MAX_GAMES) == 1 && !strcmp(media[0].title, "PPSA00006"));
    sqlite3_close(db);
  }
  // A row pinned in full is refused: one home place stays free.
  {
    static Row full[MAX_GAMES];
    sqlite3* db = open_db(0);
    int count = read_tab(db, one, GAMES_TAB, full, NULL, MAX_GAMES);
    sqlite3_close(db);
    for (int i = 0; i < count; ++i) full[i].pinned = 1;
    char backup[256], integrity[256], message[300];
    assert(save_order(one, GAMES_TAB, full, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message) == 400);
    assert(!strcmp(pins_of(one, slots), "off"));
  }
  save_digits(one, "31254", "35");
  expect_row(one, "31254");
  assert(!strcmp(pins_of(one, slots), "on") && slots[0] == 0 && slots[1] < 0 && slots[3] == 3);
  // The console raises an app while Lineup is open and lineup_pin_u renumbers the pinned ones: the row read still
  // saves while its order holds, and not once an app opened moves ahead.
  {
    static Row read[MAX_GAMES], again[MAX_GAMES];
    char backup[256], integrity[256], message[300];
    sqlite3* db = open_db(0);
    int count = read_tab(db, one, GAMES_TAB, read, NULL, MAX_GAMES);
    sqlite3_close(db);
    for (int i = 0; i < count; ++i) read[i].pinned = read[i].title[8] == '3' || read[i].title[8] == '5';
    open_game(one, "PPSA00003");
    expect_row(one, "31254");
    db = open_db(0);
    assert(read_tab(db, one, GAMES_TAB, again, NULL, MAX_GAMES) == count && again[0].index != read[0].index);
    sqlite3_close(db);
    assert(save_order(one, GAMES_TAB, read, count, backup, sizeof backup, integrity, sizeof integrity, message,
                      sizeof message) == 200);
    expect_row(one, "31254");
    open_game(one, "PPSA00004");
    expect_row(one, "34152");
    assert(save_order(one, GAMES_TAB, read, count, backup, sizeof backup, integrity, sizeof integrity, message,
                      sizeof message) == 409);
    save_digits(one, "31254", "35");
    expect_row(one, "31254");
  }
  // 1.4.2's update trigger, which ran for the Media tab too, is replaced once; the current ones stay.
  {
    static char old[16 * 1024];
    snprintf(old, sizeof old, "DROP TRIGGER lineup_pin_u_%1$s; CREATE TRIGGER lineup_pin_u_%1$s AFTER UPDATE OF "
             "lastAccessIndex ON tbl_iconinfo_%1$s WHEN NEW.lastAccessIndex > OLD.lastAccessIndex AND NOT EXISTS "
             "(SELECT 1 FROM lineup_tmp) BEGIN " REORDER("NEW.") " END;", one);
    sqlite3* db = open_db(1);
    assert(db && sqlite3_exec(db, old, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
  }
  assert(update_pin_triggers() == 1 && update_pin_triggers() == 0);
  assert(!scalar("SELECT COUNT(*) FROM sqlite_master WHERE name GLOB 'lineup_pin_[uid]_*' AND sql NOT LIKE '%" PIN_TABS "%'"));
  // Opening the Media tab's app leaves the games alone, even out of their pinned places (lowering an index fires
  // nothing); 1.4.2 would have put game 3 back first.
  long long three = scalar("SELECT lastAccessIndex FROM tbl_iconinfo_0000000001 WHERE titleId = 'PPSA00003'");
  run("UPDATE tbl_iconinfo_%1$s SET lastAccessIndex = 0 WHERE titleId = '%2$s'", one, "PPSA00003");
  open_game(one, "PPSA00006");
  expect_row(one, "12543");
  char raise[256];
  snprintf(raise, sizeof raise, "UPDATE tbl_iconinfo_%%1$s SET lastAccessIndex = %lld WHERE titleId = '%%2$s'", three);
  run(raise, one, "PPSA00003");
  expect_row(one, "31254");
  // The report lists the triggers, current, and every profile. Without tbl_contentinfo it is saved but incomplete.
  {
    char path[256], message[300], text[4096] = "";
    report_dir = "/tmp/lineupd-test-report";
    assert(write_report(path, sizeof path, message, sizeof message) == 500 && strstr(message, "was saved without it"));
    run("CREATE TABLE tbl_contentinfo (titleId TEXT, titleName TEXT, dispLocation INTEGER, viewCategory INTEGER, "
        "categoryType INTEGER, appType INTEGER, is3rdPartyVideoApp INTEGER, appinfoContentType INTEGER, contentStatus "
        "INTEGER, installStatus INTEGER, contentLocation INTEGER, pprDeeplinkUri TEXT, pprHubAppUri TEXT); ALTER TABLE tbl_iconinfo_%1$s ADD "
        "pinnedStatus INTEGER; ALTER TABLE tbl_iconinfo_%1$s ADD appDrmType INTEGER; ALTER TABLE tbl_iconinfo_%1$s ADD "
        "lastLaunchMethod INTEGER; ALTER TABLE tbl_iconinfo_%1$s ADD deeplinkUri TEXT; ALTER TABLE tbl_iconinfo_%1$s "
        "ADD hubAppUri TEXT", one, "");
    assert(write_report(path, sizeof path, message, sizeof message) == 200);
    FILE* file = fopen(path, "rb");
    assert(file);
    text[fread(text, 1, sizeof text - 1, file)] = 0;
    fclose(file);
    assert(strstr(text, "{\"name\":\"lineup_pin_u_0000000001\",\"tbl_name\":\"tbl_iconinfo_0000000001\",\"current\":1}") &&
           strstr(text, "{\"user\":\"0000000001\",\"apps\":"));
  }
  // Opening games: the others take the free places by how recently they were used.
  open_game(one, "PPSA00004");
  expect_row(one, "34152");
  open_game(one, "PPSA00002");
  expect_row(one, "32451");
  open_game(one, "PPSA00005");
  expect_row(one, "32451");
  // Installing: the new game takes the first free place.
  given = scalar("SELECT MAX(lastAccessIndex) FROM tbl_iconinfo_0000000001") + 1;
  char sql[256];
  snprintf(sql, sizeof sql, "INSERT INTO tbl_iconinfo_%%1$s (titleId, titleName, lastAccessIndex, visible, dispLocation) "
           "VALUES ('%%2$s','Seven',%lld,1,138)", given);
  run(sql, one, "PPSA00007");
  expect_row(one, "372541");
  // Uninstalling: a game pinned past the end of the shorter row goes last.
  run("DELETE FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", one, "PPSA00007");
  run("DELETE FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", one, "PPSA00004");
  expect_row(one, "3215");
  run("DELETE FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", one, "PPSA00001");
  expect_row(one, "325");
  // Saving again moves a pinned game and keeps it pinned at its new place.
  save_digits(one, "235", "35");
  open_game(one, "PPSA00005");
  expect_row(one, "235");
  open_game(one, "PPSA00002");
  expect_row(one, "235");
  assert(!strcmp(pins_of(one, slots), "on") && slots[1] == 1 && slots[0] < 0 && slots[2] == 2);
  // The Media tab (6, then 7, 8 and 9) keeps its own pins, apart from the games'.
  run("INSERT INTO tbl_iconinfo_%1$s (titleId, titleName, lastAccessIndex, visible, dispLocation) VALUES "
      "('PPSA00017','Seventeen',3,1,188),('PPSA00018','Eighteen',2,1,188),('PPSA00019','Nineteen',1,1,188)", one, "");
  expect_tab(one, MEDIA_TAB, "6789");
  {
    static Row full[MAX_GAMES];
    sqlite3* db = open_db(0);
    int count = read_tab(db, one, MEDIA_TAB, full, NULL, MAX_GAMES);
    sqlite3_close(db);
    for (int i = 0; i < count; ++i) full[i].pinned = 1;
    char backup[256], integrity[256], message[300];
    assert(save_order(one, MEDIA_TAB, full, count, backup, sizeof backup, integrity, sizeof integrity, message,
                      sizeof message) == 400);
  }
  save_tab(one, MEDIA_TAB, "9678", "9");
  expect_tab(one, MEDIA_TAB, "9678");
  assert(!strcmp(tab_pins_of(one, MEDIA_TAB, slots), "on") && slots[0] == 0 && slots[1] < 0);
  open_game(one, "PPSA00018");
  expect_tab(one, MEDIA_TAB, "9867");
  expect_row(one, "235");
  open_game(one, "PPSA00002");
  expect_row(one, "235");
  expect_tab(one, MEDIA_TAB, "9867");
  // Saving one tab leaves the other's pins alone, even with none left on the tab saved.
  save_digits(one, "235", "3");
  open_game(one, "PPSA00006");
  expect_tab(one, MEDIA_TAB, "9687");
  save_tab(one, MEDIA_TAB, "9687", "");
  open_game(one, "PPSA00005");
  expect_row(one, "532");
  open_game(one, "PPSA00017");
  expect_tab(one, MEDIA_TAB, "7968");
  save_digits(one, "235", "35");
  // Without its triggers (a rebuilt database), the pins show as missing.
  run("DROP TRIGGER lineup_pin_d_%1$s", one, "");
  assert(!strcmp(pins_of(one, slots), "missing"));

  char backup[256], message[300];
  // Hiding takes an app off its tab and out of the pins; showing brings back only what Lineup hid.
  assert(set_hidden(one, TITLE_ID, 1, backup, sizeof backup, message, sizeof message) == 400);
  assert(set_hidden(one, "PPSA00005", 0, backup, sizeof backup, message, sizeof message) == 404);
  assert(set_hidden(one, "PPSA00005", 1, backup, sizeof backup, message, sizeof message) == 200);
  expect_row(one, "23");
  assert(!scalar("SELECT COUNT(*) FROM lineup_pin WHERE titleId = 'PPSA00005'"));
  assert(set_hidden(one, "PPSA00005", 1, backup, sizeof backup, message, sizeof message) == 409);
  run("INSERT INTO tbl_iconinfo_%1$s (titleId, titleName, lastAccessIndex, visible, dispLocation) VALUES "
      "('NPXS40047','Store',1,1,146),('NPXS40037','Plus',1,0,147),('NPXS40071','Library',1,1,146),"
      "('NPXS40010','Off',1,1,128)", one, "");
  assert(set_hidden(one, "NPXS40071", 1, backup, sizeof backup, message, sizeof message) == 400);
  // A system tile off both tabs never shows on the home screen, so it does not hide either.
  assert(set_hidden(one, "NPXS40010", 1, backup, sizeof backup, message, sizeof message) == 409);
  assert(set_hidden(one, "NPXS40037", 0, backup, sizeof backup, message, sizeof message) == 409);
  assert(set_hidden(one, STORE_TITLE, 1, backup, sizeof backup, message, sizeof message) == 200);
  assert(scalar("SELECT COUNT(*) FROM lineup_hidden") == 2);
  run("INSERT INTO tbl_iconinfo_%1$s (titleId, titleName, lastAccessIndex, visible, dispLocation) VALUES ('%2$s','Gallery',1,1,154)", one, GALLERY_TITLE);
  assert(set_hidden(one, GALLERY_TITLE, 1, backup, sizeof backup, message, sizeof message) == 200);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE titleId = '%2$s'", one, GALLERY_TITLE);
  assert(!scalar("SELECT visible FROM tbl_iconinfo_0000000001 WHERE titleId = '" GALLERY_TITLE "'"));
  assert(set_hidden(one, GALLERY_TITLE, 0, backup, sizeof backup, message, sizeof message) == 200);
  assert(scalar("SELECT visible FROM tbl_iconinfo_0000000001 WHERE titleId = '" GALLERY_TITLE "'") == 1);
  // The Gallery visible at 154 is on the Games row now; the system takes it off for the checks of the row below.
  run("UPDATE tbl_iconinfo_%1$s SET visible = 0 WHERE titleId = '%2$s'", one, GALLERY_TITLE);
  // The console brings a hidden app back with an update or a new row; the triggers take it away, and only it.
  const char* store_visible = "SELECT visible FROM tbl_iconinfo_0000000001 WHERE titleId = '" STORE_TITLE "'";
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE titleId = '%2$s'", one, STORE_TITLE);
  assert(!scalar(store_visible));
  run("DELETE FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", one, STORE_TITLE);
  run("INSERT INTO tbl_iconinfo_%1$s (titleId, titleName, lastAccessIndex, visible, dispLocation) VALUES ('%2$s','Store',1,1,146)", one, STORE_TITLE);
  assert(!scalar(store_visible));
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE titleId = '%2$s'", one, "NPXS40037");
  assert(scalar("SELECT visible FROM tbl_iconinfo_0000000001 WHERE titleId = 'NPXS40037'") == 1);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 0 WHERE titleId = '%2$s'", one, "NPXS40037");
  // As left by 1.6.0: no triggers and the Store back. Opening Lineup hides it again, once.
  run("DROP TRIGGER lineup_hide_u_%1$s; DROP TRIGGER lineup_hide_i_%1$s; UPDATE tbl_iconinfo_%1$s SET visible = 1 "
      "WHERE titleId = '%2$s'", one, STORE_TITLE);
  assert(scalar(store_visible) == 1);
  assert(restore_hidden() == 1 && restore_hidden() == 0);
  assert(!scalar(store_visible) && has_object_named("lineup_hide_u_0000000001") && has_object_named("lineup_hide_i_0000000001"));
  assert(set_hidden(one, "PPSA00005", 0, backup, sizeof backup, message, sizeof message) == 200);
  expect_row(one, "235");
  assert(has_object_named("lineup_hide_u_0000000001"));
  assert(set_hidden(one, STORE_TITLE, 0, backup, sizeof backup, message, sizeof message) == 200);
  assert(scalar(store_visible) == 1);
  assert(!has_object_named("lineup_hidden") && !has_object_named("lineup_hide_u_0000000001") &&
         !has_object_named("lineup_hide_i_0000000001"));
  // A shown app stays shown.
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE titleId = '%2$s'", one, STORE_TITLE);
  assert(scalar(store_visible) == 1);
  // Removing Lineup takes the triggers before their table.
  assert(set_hidden(one, STORE_TITLE, 1, backup, sizeof backup, message, sizeof message) == 200);
  assert(!show_all_of_test() && !has_object_named("lineup_hide_u_0000000001") && !has_object_named("lineup_hidden"));
  assert(scalar(store_visible) == 1);
  run("DELETE FROM tbl_iconinfo_%1$s WHERE titleId LIKE 'NPXS%%'", one, "");
  // Moving: every place agrees, the rest of the category survives, the pin goes, and both databases roll back together.
  {
    sqlite3* info;
    unlink(appinfo_path);
    assert(sqlite3_open(appinfo_path, &info) == SQLITE_OK);
    assert(sqlite3_exec(info, "CREATE TABLE tbl_appinfo (titleId TEXT, metaDataId TEXT, key TEXT, val BLOB);"
      "INSERT INTO tbl_appinfo VALUES ('PPSA00002','m','DISPLAYLOCATION',138),('PPSA00002','m','CATEGORY_TYPE',512),"
      "('PPSA00002','m','HUBAPP_URI','pshome:gamehub?titleId=PPSA00002'),('PPSA00002','m','TITLE','Two'),"
      "('PPSA00003','m','DISPLAYLOCATION',138),('PPSA00003','m','CATEGORY_TYPE',0),"
      "('PPSA99077','m','DISPLAYLOCATION',138),('PPSA99077','m','CATEGORY_TYPE',0),"
      "('PPSA99077','m','HUBAPP_URI','pshome:gamehub?titleId=PPSA99077')", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(info);
    run("ALTER TABLE tbl_contentinfo ADD AppInfoJson TEXT;"
        "INSERT INTO tbl_contentinfo (titleId, dispLocation, categoryType, appType, is3rdPartyVideoApp, pprHubAppUri, "
        "contentLocation, AppInfoJson) VALUES ('PPSA00002',138,512,5,0,'pshome:gamehub?titleId=PPSA00002',2,"
        "'{\"DISPLAYLOCATION\":138,\"CATEGORY_TYPE\":512,\"HUBAPP_URI\":\"x\",\"URL\":\"http:\\/\\/a\"}'),"
        "('PPSA00003',138,0,5,0,'pshome:gamehub?titleId=PPSA00003',2,'{}'),('PPSA00005',138,0,1,0,'g',2,'{}'),"
        "('PPSA00018',188,65536,5,1,'m',0,'{}'),('PPSA99077',138,0,5,0,'pshome:gamehub?titleId=PPSA99077',2,'{}')", one, "");
    save_digits(one, "235", "2");
    {
      sqlite3* db = open_db(0);
      assert(is_movable(db, "PPSA00002") && !is_movable(db, "PPSA00005") && !is_movable(db, "PPSA00018") &&
             is_movable(db, TITLE_ID) && !is_movable(db, "PPSA09999"));
      sqlite3_close(db);
    }
    assert(move_app("PPSA00005", MEDIA_TAB, backup, sizeof backup, message, sizeof message) == 400);
    assert(move_app("PPSA00018", GAMES_TAB, backup, sizeof backup, message, sizeof message) == 400);
    assert(move_app("PPSA00002", GAMES_TAB, backup, sizeof backup, message, sizeof message) == 409);
    // Without all three appinfo rows nothing changes, in either database.
    assert(move_app("PPSA00003", MEDIA_TAB, backup, sizeof backup, message, sizeof message) == 500);
    assert(scalar("SELECT dispLocation FROM tbl_contentinfo WHERE titleId = 'PPSA00003'") == GAMES_TAB);
    assert(scalar("SELECT dispLocation FROM tbl_iconinfo_0000000001 WHERE titleId = 'PPSA00003'") == GAMES_TAB);
    assert(move_app("PPSA00002", MEDIA_TAB, backup, sizeof backup, message, sizeof message) == 200);
    assert(scalar("SELECT COUNT(*) FROM tbl_iconinfo_0000000001 WHERE titleId = 'PPSA00002' AND dispLocation = 188 AND "
                  "hubAppUri = 'psmediahub:main?titleId=PPSA00002'") == 1);
    assert(scalar("SELECT COUNT(*) FROM tbl_contentinfo WHERE titleId = 'PPSA00002' AND dispLocation = 188 AND "
                  "categoryType = 66048 AND is3rdPartyVideoApp = 1 AND pprHubAppUri = 'psmediahub:main?titleId=PPSA00002' AND "
                  "AppInfoJson ->> '$.DISPLAYLOCATION' = 188 AND AppInfoJson ->> '$.CATEGORY_TYPE' = 66048 AND "
                  "AppInfoJson ->> '$.HUBAPP_URI' = 'psmediahub:main?titleId=PPSA00002' AND "
                  "AppInfoJson ->> '$.URL' = 'http://a'") == 1);
    assert(!has_object_named("lineup_pin"));
    expect_row(one, "35");
    // Installing it again, as Game Compressor does each time it starts, writes it as a game: the triggers keep it on
    // Media everywhere but AppInfoJson, which restore_moved fixes, once.
    run("DELETE FROM tbl_contentinfo WHERE titleId = '%2$s'; DELETE FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s';"
        "INSERT INTO tbl_contentinfo (titleId, dispLocation, categoryType, appType, is3rdPartyVideoApp, pprHubAppUri, "
        "contentLocation, AppInfoJson) VALUES ('%2$s',138,512,5,0,'pshome:gamehub?titleId=%2$s',2,'{\"DISPLAYLOCATION\""
        ":138,\"CATEGORY_TYPE\":512,\"HUBAPP_URI\":\"pshome:gamehub?titleId=%2$s\"}');"
        "INSERT INTO tbl_iconinfo_%1$s (titleId, titleName, lastAccessIndex, visible, dispLocation, hubAppUri) VALUES "
        "('%2$s','Two',1,1,138,'pshome:gamehub?titleId=%2$s')", one, "PPSA00002");
    run_info("DELETE FROM tbl_appinfo WHERE titleId = 'PPSA00002'; INSERT INTO tbl_appinfo VALUES "
             "('PPSA00002','m','DISPLAYLOCATION',138),('PPSA00002','m','CATEGORY_TYPE',512),"
             "('PPSA00002','m','HUBAPP_URI','pshome:gamehub?titleId=PPSA00002'),('PPSA00002','m','TITLE','Two')");
    const char* content_moved = "SELECT COUNT(*) FROM tbl_contentinfo WHERE titleId = 'PPSA00002' AND dispLocation = 188 "
                                "AND categoryType = 66048 AND is3rdPartyVideoApp = 1 AND pprHubAppUri = "
                                "'psmediahub:main?titleId=PPSA00002'";
    const char* icon_moved = "SELECT COUNT(*) FROM tbl_iconinfo_0000000001 WHERE titleId = 'PPSA00002' AND dispLocation = "
                             "188 AND hubAppUri = 'psmediahub:main?titleId=PPSA00002'";
    const char* json_tab = "SELECT AppInfoJson ->> '$.DISPLAYLOCATION' FROM tbl_contentinfo WHERE titleId = 'PPSA00002'";
    const char* info_moved = "188|66048|psmediahub:main?titleId=PPSA00002|Two";
    assert(scalar(content_moved) == 1 && scalar(icon_moved) == 1 && !strcmp(info_values("PPSA00002"), info_moved));
    assert(scalar(json_tab) == GAMES_TAB);
    assert(restore_moved() == 1 && restore_moved() == 0);
    assert(scalar(json_tab) == MEDIA_TAB && scalar(content_moved) == 1);
    // Or the system changes the tab where it is.
    run("UPDATE tbl_contentinfo SET dispLocation = 138 WHERE titleId = '%2$s'; UPDATE tbl_iconinfo_%1$s SET "
        "dispLocation = 138 WHERE titleId = '%2$s'", one, "PPSA00002");
    run_info("UPDATE tbl_appinfo SET val = 138 WHERE titleId = 'PPSA00002' AND key = 'DISPLAYLOCATION';"
             "UPDATE tbl_appinfo SET val = 512 WHERE titleId = 'PPSA00002' AND key = 'CATEGORY_TYPE'");
    assert(scalar(content_moved) == 1 && scalar(icon_moved) == 1 && !strcmp(info_values("PPSA00002"), info_moved));
    // Apps Lineup did not move stay where the system puts them.
    run("UPDATE tbl_contentinfo SET dispLocation = 188 WHERE titleId = 'PPSA00003'", one, "");
    assert(scalar("SELECT dispLocation FROM tbl_contentinfo WHERE titleId = 'PPSA00003'") == MEDIA_TAB);
    run("UPDATE tbl_contentinfo SET dispLocation = 138 WHERE titleId = 'PPSA00003'", one, "");
    // A profile made after the move gets its triggers, and the app its tab there, when Lineup starts.
    run("CREATE TABLE tbl_iconinfo_%1$s (titleId TEXT, titleName TEXT, lastAccessIndex INTEGER, visible INTEGER, "
        "dispLocation INTEGER, hubAppUri TEXT); INSERT INTO tbl_iconinfo_%1$s VALUES ('%2$s','Two',1,1,138,'g')",
        "0000000003", "PPSA00002");
    assert(restore_moved() == 1 && restore_moved() == 0);
    assert(scalar("SELECT dispLocation FROM tbl_iconinfo_0000000003 WHERE titleId = 'PPSA00002'") == MEDIA_TAB);
    assert(has_object_named("lineup_move_i_0000000003") && has_object_named("lineup_move_u_0000000003"));
    run("DROP TABLE tbl_iconinfo_%1$s", "0000000003", "");
    assert(move_app("PPSA00002", GAMES_TAB, backup, sizeof backup, message, sizeof message) == 200);
    assert(scalar("SELECT categoryType FROM tbl_contentinfo WHERE titleId = 'PPSA00002'") == 512);
    // Back on the tab it started on, nothing of the moves stays, in either database.
    assert(!scalar("SELECT COUNT(*) FROM sqlite_master WHERE name GLOB 'lineup_move*'"));
    assert(!strcmp(info_values("lineup_move%"), ""));
    assert(sqlite3_open(appinfo_path, &info) == SQLITE_OK);
    sqlite3_stmt* statement;
    assert(sqlite3_prepare_v2(info, "SELECT group_concat(val, '|') FROM (SELECT val FROM tbl_appinfo WHERE titleId = "
                              "'PPSA00002' ORDER BY rowid)", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    assert(!strcmp((const char*)sqlite3_column_text(statement, 0), "138|512|pshome:gamehub?titleId=PPSA00002|Two"));
    sqlite3_finalize(statement);
    sqlite3_close(info);
    // Lineup moves too. /pins/uninstall lets go of the moves and leaves it where it is.
    assert(move_app(TITLE_ID, MEDIA_TAB, backup, sizeof backup, message, sizeof message) == 200);
    assert(scalar("SELECT COUNT(*) FROM lineup_moved WHERE titleId = '" TITLE_ID "' AND tab = 188 AND home = 138") == 1);
    assert(unpin(NULL, backup, sizeof backup, message, sizeof message) == 200);
    assert(!scalar("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'lineup%'") && !strcmp(info_values("lineup%"), ""));
    assert(scalar("SELECT dispLocation FROM tbl_contentinfo WHERE titleId = '" TITLE_ID "'") == MEDIA_TAB);
    save_digits(one, "235", "3");
    run("DELETE FROM tbl_contentinfo", one, "");
  }

  // /pins/off: the profile's pins and, the last ones gone, every lineup_* object; the system's order is back.
  assert(unpin(one, backup, sizeof backup, message, sizeof message) == 200);
  assert(!strcmp(pins_of(one, slots), "off") && !scalar("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'lineup%'"));
  open_game(one, "PPSA00005");
  expect_row(one, "523");

  // /pins/uninstall with two profiles pinned.
  const char* two = "0000000002";
  run("CREATE TABLE tbl_iconinfo_%1$s (titleId TEXT, titleName TEXT, lastAccessIndex INTEGER, visible INTEGER, "
      "dispLocation INTEGER); INSERT INTO tbl_iconinfo_%1$s VALUES ('PPSA00008','Eight',2,1,138),('PPSA00009','Nine',1,1,138)",
      two, "");
  save_digits(one, "523", "2");
  save_digits(two, "98", "8");
  assert(unpin(one, backup, sizeof backup, message, sizeof message) == 200);
  assert(!strcmp(pins_of(two, slots), "on") && scalar("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'lineup%'") == 6);
  save_digits(one, "523", "2");
  // /pins/uninstall also brings back what Lineup hid.
  assert(set_hidden(two, "PPSA00009", 1, backup, sizeof backup, message, sizeof message) == 200);
  assert(unpin(NULL, backup, sizeof backup, message, sizeof message) == 200);
  assert(scalar("SELECT visible FROM tbl_iconinfo_0000000002 WHERE titleId = 'PPSA00009'") == 1);
  assert(!strcmp(pins_of(one, slots), "off") && !strcmp(pins_of(two, slots), "off"));
  assert(!scalar("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'lineup%'"));
}

static long long info_scalar(const char* sql) {
  sqlite3* info;
  sqlite3_stmt* statement;
  assert(sqlite3_open(appinfo_path, &info) == SQLITE_OK);
  assert(sqlite3_prepare_v2(info, sql, -1, &statement, NULL) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW);
  long long value = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(info);
  return value;
}

// `sql` with %1$s the user and %2$s the title, as scalar.
static long long scalar_of(const char* format, const char* user, const char* title) {
  char sql[1024];
  snprintf(sql, sizeof sql, format, user, title);
  return scalar(sql);
}

// Every place of system tile `title` holds `location` (`concept` on tbl_concepticoninfo_<user>), JSON included.
static void expect_system(const char* user, const char* title, int location, int concept) {
  char sql[400];
  assert(scalar_of("SELECT dispLocation FROM tbl_contentinfo WHERE titleId = '%2$s'", user, title) == location);
  assert(scalar_of("SELECT AppInfoJson ->> '$.DISPLAYLOCATION' FROM tbl_contentinfo WHERE titleId = '%2$s'", user,
                   title) == location);
  assert(scalar_of("SELECT dispLocation FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, title) == location);
  assert(scalar_of("SELECT dispLocation FROM tbl_concepticoninfo_%1$s WHERE localConceptId = 'cid:local:%2$s'", user,
                   title) == concept);
  assert(scalar_of("SELECT dispLocation FROM tbl_conceptmetadata WHERE localConceptId = 'cid:local:%2$s'", user,
                   title) == location);
  assert(scalar_of("SELECT e.value ->> '$.data' FROM tbl_conceptmetadata m, json_each(m.ConceptInfoJson, "
                   "'$.field_list') e WHERE m.localConceptId = 'cid:local:%2$s' AND e.value ->> '$.key' = "
                   "'display_location'", user, title) == location);
  snprintf(sql, sizeof sql, "SELECT val FROM tbl_appinfo WHERE titleId = '%s' AND key = 'DISPLAYLOCATION'", title);
  assert(info_scalar(sql) == location);
  snprintf(sql, sizeof sql, "SELECT val FROM tbl_conceptinfo WHERE local_concept_id = 'cid:local:%s' AND key = "
           "'display_location'", title);
  assert(info_scalar(sql) == location);
}

static int integrity_ok(const char* path) {
  sqlite3* db;
  char check[128];
  assert(sqlite3_open(path, &db) == SQLITE_OK);
  int ok = !pragma_text(db, "PRAGMA integrity_check", check, sizeof check);
  sqlite3_close(db);
  return ok;
}

// A synthetic pair of databases with the console's columns for the PS Store, the Media Gallery, Game Library, App
// Library, Game Compressor (moved to Media by another tool, its concept left on Games) and TabShifter's tile.
static void make_system_databases(const char* user) {
  unlink(db_path);
  unlink(appinfo_path);
  sqlite3* created;
  assert(sqlite3_open(db_path, &created) == SQLITE_OK);
  sqlite3_close(created);
  run("CREATE TABLE tbl_iconinfo_%1$s (titleId TEXT, titleName TEXT, lastAccessIndex INTEGER, visible INTEGER, "
      "dispLocation INTEGER, hubAppUri TEXT, localConceptId TEXT);"
      "CREATE TABLE tbl_concepticoninfo_%1$s (localConceptId TEXT, primaryTitleId TEXT, dispLocation INTEGER, hubAppUri TEXT);"
      "CREATE TABLE tbl_contentinfo (titleId TEXT, localConceptId TEXT, dispLocation INTEGER, categoryType INTEGER, "
      "appType INTEGER, contentLocation INTEGER, is3rdPartyVideoApp INTEGER, pprHubAppUri TEXT, AppInfoJson TEXT);"
      "CREATE TABLE tbl_conceptmetadata (localConceptId TEXT, dispLocation INTEGER, pprHubAppUri TEXT, ConceptInfoJson TEXT);"
      "CREATE TABLE tbl_group_%1$s (itemId TEXT, groupName TEXT, type TEXT, idx INTEGER);"
      "CREATE TABLE tbl_info_%1$s (key TEXT PRIMARY KEY, value TEXT);"
      "INSERT INTO tbl_group_%1$s VALUES ('PPSA00001','MyHome','title',1),('NPXS40075','MyHome','title',2),"
      "('NPXS40071','MyHome','title',3);"
      "INSERT INTO tbl_info_%1$s VALUES ('nsx_content_flag_PPSA00001','{\"deleted\":true,\"pushed\":false}')",
      user, "");
  struct { const char* title; int location, concept, category; const char* hub; } tiles[] = {
    {"NPXS40047", 146, 2194, 67108864, "pshome:gamestore"}, {"NPXS40075", 154, 2202, 67108864, "psmediagalleryhub:main"},
    {"NPXS40071", 146, 2194, 67108864, "pslibraryhub:main?space=game"},
    {"NPXS40139", 148, 2196, 67108864, "pslibraryhub:main?space=media"},
    {"PSGC50001", 188, 1162, 0, "psmediahub:main?titleId=PSGC50001"},
    {"TBSH00001", 188, 2236, 65536, "psmediahub:main?titleId=TBSH00001"},
  };
  for (size_t i = 0; i < sizeof tiles / sizeof tiles[0]; ++i) {
    char sql[1800], info[700];
    // Game Compressor's concept icon still holds its Games tab and hub, as on the console.
    const char* concept_hub = !strcmp(tiles[i].title, "PSGC50001") ? "pshome:gamehub?titleId=PSGC50001" : tiles[i].hub;
    snprintf(sql, sizeof sql,
      "INSERT INTO tbl_iconinfo_%%1$s VALUES ('%1$s','%1$s',%7$d,1,%2$d,'%5$s','cid:local:%1$s');"
      "INSERT INTO tbl_concepticoninfo_%%1$s VALUES ('cid:local:%1$s','%1$s',%3$d,'%6$s');"
      "INSERT INTO tbl_contentinfo VALUES ('%1$s','cid:local:%1$s',%2$d,%4$d,0,2,0,'%5$s',"
      "'{\"DISPLAYLOCATION\":%2$d,\"CATEGORY_TYPE\":%4$d,\"HUBAPP_URI\":\"%5$s\",\"SIZE\":9007199254740993}');"
      "INSERT INTO tbl_conceptmetadata VALUES ('cid:local:%1$s',%2$d,'%5$s','{\"field_list\":[{\"data\":\"%1$s\","
      "\"key\":\"concept_name\",\"size\":9,\"type\":2},{\"data\":%2$d,\"key\":\"display_location\",\"size\":8,\"type\":0}]}')",
      tiles[i].title, tiles[i].location, tiles[i].concept, tiles[i].category, tiles[i].hub, concept_hub, (int)i + 1);
    run(sql, user, "");
    snprintf(info, sizeof info, "INSERT INTO tbl_appinfo VALUES ('%1$s','m','DISPLAYLOCATION',%2$d),"
             "('%1$s','m','CATEGORY_TYPE',%3$d),('%1$s','m','HUBAPP_URI','%4$s');"
             "INSERT INTO tbl_conceptinfo VALUES ('cid:local:%1$s','m','display_location',%2$d),"
             "('cid:local:%1$s','m','hubapp_uri','%4$s')", tiles[i].title, tiles[i].location, tiles[i].category,
             tiles[i].hub);
    if (!i) run_info("CREATE TABLE tbl_appinfo (titleId TEXT, metaDataId TEXT, key INT, val);"
                     "CREATE TABLE tbl_conceptinfo (local_concept_id TEXT, metadata_id TEXT, key INT, val);");
    run_info(info);
  }
  run("INSERT INTO tbl_iconinfo_%1$s VALUES ('NPXS40063','Welcome',0,0,147,'pshome:explorehub','cid:local:NPXS40063');"
      "INSERT INTO tbl_contentinfo VALUES ('NPXS40063','cid:local:NPXS40063',147,67108864,0,2,0,'pshome:explorehub',"
      "'{\"DISPLAYLOCATION\":147}');"
      "INSERT INTO tbl_conceptmetadata VALUES ('cid:local:NPXS40063',147,'pshome:explorehub','{\"field_list\":"
      "[{\"data\":147,\"key\":\"display_location\",\"size\":8,\"type\":0}]}')", user, "");
  run_info("INSERT INTO tbl_appinfo VALUES ('NPXS40063','m','DISPLAYLOCATION',147);"
           "INSERT INTO tbl_conceptinfo VALUES ('cid:local:NPXS40063','m','display_location',147)");
}

// Every place of Welcome holds `location`, and its tile `visible` on `user`.
static void expect_welcome(const char* user, int location, int visible) {
  assert(scalar_of("SELECT dispLocation FROM tbl_contentinfo WHERE titleId = '%2$s'", user, WELCOME_TITLE) == location);
  assert(scalar_of("SELECT AppInfoJson ->> '$.DISPLAYLOCATION' FROM tbl_contentinfo WHERE titleId = '%2$s'", user,
                   WELCOME_TITLE) == location);
  assert(scalar_of("SELECT dispLocation FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, WELCOME_TITLE) == location);
  assert(scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, WELCOME_TITLE) == visible);
  assert(scalar_of("SELECT dispLocation FROM tbl_conceptmetadata WHERE localConceptId = 'cid:local:%2$s'", user,
                   WELCOME_TITLE) == location);
  assert(info_scalar("SELECT val FROM tbl_appinfo WHERE titleId = 'NPXS40063' AND key = 'DISPLAYLOCATION'") == location);
  assert(info_scalar("SELECT val FROM tbl_conceptinfo WHERE local_concept_id = 'cid:local:NPXS40063' AND key = "
                     "'display_location'") == location);
}

// The titles of tab `location` on `user`, in home order, into `rows`. Returns how many.
static int tab_rows(const char* user, int location, Row* rows) {
  sqlite3* db = open_db(0);
  int count = read_tab(db, user, location, rows, NULL, MAX_GAMES);
  sqlite3_close(db);
  return count;
}

// System tiles, the concept places of moved apps, TabShifter's tile and the first backups, on `user` of the databases
// at db_path and appinfo_path: the synthetic ones, or copies of a console's.
static void test_system(const char* user) {
  char backup[256], message[300], path[300];
  // The first start copies both databases aside once; a second start keeps that first copy.
  assert(!system("rm -f /tmp/lineupd-test-backups/original-*"));
  assert(!backup_originals());
  snprintf(path, sizeof path, "%s/original-app.db", backup_dir);
  assert(integrity_ok(path));
  snprintf(path, sizeof path, "%s/original-appinfo.db", backup_dir);
  assert(integrity_ok(path));
  struct stat first, again;
  assert(!stat(path, &first) && !backup_originals() && !stat(path, &again) && first.st_mtime == again.st_mtime);

  // The libraries never move, and Game Library never hides; App Library hides, stays hidden when the system shows it
  // again, and comes back.
  assert(move_system("NPXS40071", 1, backup, sizeof backup, message, sizeof message) == 400);
  assert(move_system("NPXS40139", 0, backup, sizeof backup, message, sizeof message) == 400);
  assert(set_hidden(user, "NPXS40071", 1, backup, sizeof backup, message, sizeof message) == 400);
  assert(set_hidden(user, APP_LIBRARY_TITLE, 1, backup, sizeof backup, message, sizeof message) == 200);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE titleId = '%2$s'", user, APP_LIBRARY_TITLE);
  assert(!scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, APP_LIBRARY_TITLE));
  assert(move_system(APP_LIBRARY_TITLE, 0, backup, sizeof backup, message, sizeof message) == 400);
  assert(set_hidden(user, APP_LIBRARY_TITLE, 0, backup, sizeof backup, message, sizeof message) == 200);
  assert(scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, APP_LIBRARY_TITLE) == 1);
  expect_system(user, APP_LIBRARY_TITLE, 148, 2196);
  assert(set_hidden(user, WELCOME_TITLE, 1, backup, sizeof backup, message, sizeof message) == 400);
  assert(move_app("NPXS40071", MEDIA_TAB, backup, sizeof backup, message, sizeof message) != 200);

  // The Store does not move; the Gallery to Media: one bit for the other, everywhere, each backup with appinfo.db's
  // beside it.
  expect_system(user, STORE_TITLE, 146, 2194);
  assert(move_system(STORE_TITLE, 1, backup, sizeof backup, message, sizeof message) == 400);
  assert(move_system(GALLERY_TITLE, 0, backup, sizeof backup, message, sizeof message) == 409);
  // The Gallery's place in MyHome counted from 1, 0 when it is not there; the group's size.
  const char* home_place = "SELECT COUNT(*) FROM tbl_group_%1$s g WHERE g.groupName = 'MyHome' AND g.idx <= (SELECT idx "
                           "FROM tbl_group_%1$s WHERE groupName = 'MyHome' AND itemId = '%2$s')";
  const char* home_size = "SELECT COUNT(*) FROM tbl_group_%1$s WHERE groupName = 'MyHome'";
  long long gallery_home = scalar_of(home_place, user, GALLERY_TITLE), home_count = scalar_of(home_size, user, "");
  assert(move_system(GALLERY_TITLE, 1, backup, sizeof backup, message, sizeof message) == 200);
  snprintf(path, sizeof path, "%.*s.appinfo.db", (int)strlen(backup) - 3, backup);
  assert(integrity_ok(backup) && integrity_ok(path));
  expect_system(user, GALLERY_TITLE, 156, 2204);
  expect_system(user, STORE_TITLE, 146, 2194);
  assert(!scalar_of("SELECT COUNT(*) FROM tbl_group_%1$s WHERE itemId = '%2$s'", user, GALLERY_TITLE));
  assert(move_system(GALLERY_TITLE, 1, backup, sizeof backup, message, sizeof message) == 409);
  // A big number in the JSON comes through as it was written.
  assert(scalar_of("SELECT instr(AppInfoJson, '9007199254740993') > 0 OR json_type(AppInfoJson, '$.SIZE') IS NULL "
                   "FROM tbl_contentinfo WHERE titleId = '%2$s'", user, GALLERY_TITLE));

  // The console writes the Gallery back on Games, in place or anew, as at a restart: the triggers keep it on Media.
  run("UPDATE tbl_contentinfo SET dispLocation = 154 WHERE titleId = '%2$s';"
      "UPDATE tbl_iconinfo_%1$s SET dispLocation = 154 WHERE titleId = '%2$s';"
      "UPDATE tbl_concepticoninfo_%1$s SET dispLocation = 2202 WHERE localConceptId = 'cid:local:%2$s';"
      "UPDATE tbl_conceptmetadata SET dispLocation = 154 WHERE localConceptId = 'cid:local:%2$s'", user, GALLERY_TITLE);
  run_info("UPDATE tbl_appinfo SET val = 154 WHERE titleId = 'NPXS40075' AND key = 'DISPLAYLOCATION';"
           "UPDATE tbl_conceptinfo SET val = 154 WHERE local_concept_id = 'cid:local:NPXS40075' AND key = 'display_location'");
  assert(scalar_of("SELECT dispLocation FROM tbl_contentinfo WHERE titleId = '%2$s'", user, GALLERY_TITLE) == 156);
  assert(scalar_of("SELECT dispLocation FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, GALLERY_TITLE) == 156);
  assert(scalar_of("SELECT dispLocation FROM tbl_concepticoninfo_%1$s WHERE localConceptId = 'cid:local:%2$s'", user,
                   GALLERY_TITLE) == 2204);
  assert(info_scalar("SELECT val FROM tbl_appinfo WHERE titleId = 'NPXS40075' AND key = 'DISPLAYLOCATION'") == 156);
  run("CREATE TEMP TABLE again AS SELECT * FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s';"
      "DELETE FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'; UPDATE again SET dispLocation = 154;"
      "INSERT INTO tbl_iconinfo_%1$s SELECT * FROM again", user, GALLERY_TITLE);
  assert(scalar_of("SELECT dispLocation FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, GALLERY_TITLE) == 156);
  // A tile the system takes off both tabs stays off; written on Games again, it goes back to Media.
  run("UPDATE tbl_iconinfo_%1$s SET dispLocation = 1 WHERE titleId = '%2$s'", user, GALLERY_TITLE);
  assert(scalar_of("SELECT dispLocation FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, GALLERY_TITLE) == 1);
  run("UPDATE tbl_iconinfo_%1$s SET dispLocation = 154 WHERE titleId = '%2$s'", user, GALLERY_TITLE);
  assert(scalar_of("SELECT dispLocation FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, GALLERY_TITLE) == 156);
  // The JSON the triggers leave is put right when Lineup starts.
  run("UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', 154) WHERE titleId = '%2$s'",
      user, GALLERY_TITLE);
  assert(restore_system() == 1 && restore_system() == 0);
  expect_system(user, GALLERY_TITLE, 156, 2204);
  // Hidden on Media, the Gallery stays hidden there; the Store hides on Games and stays there.
  assert(set_hidden(user, GALLERY_TITLE, 1, backup, sizeof backup, message, sizeof message) == 200);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1, dispLocation = 154 WHERE titleId = '%2$s'", user, GALLERY_TITLE);
  assert(!scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, GALLERY_TITLE));
  assert(scalar_of("SELECT dispLocation FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, GALLERY_TITLE) == 156);
  assert(set_hidden(user, GALLERY_TITLE, 0, backup, sizeof backup, message, sizeof message) == 200);
  assert(set_hidden(user, STORE_TITLE, 1, backup, sizeof backup, message, sizeof message) == 200);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE titleId = '%2$s'", user, STORE_TITLE);
  assert(!scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, STORE_TITLE));
  expect_system(user, STORE_TITLE, 146, 2194);
  assert(set_hidden(user, STORE_TITLE, 0, backup, sizeof backup, message, sizeof message) == 200);
  // A Store an earlier test build moved to Media goes back to Games when Lineup starts, and leaves lineup_system;
  // the Gallery keeps its row.
  run("INSERT INTO lineup_system VALUES ('%2$s','cid:local:%2$s',4,2,2,0);"
      "UPDATE tbl_contentinfo SET dispLocation = 148 WHERE titleId = '%2$s';"
      "UPDATE tbl_iconinfo_%1$s SET dispLocation = 148 WHERE titleId = '%2$s';"
      "UPDATE tbl_concepticoninfo_%1$s SET dispLocation = 2196 WHERE localConceptId = 'cid:local:%2$s';"
      "UPDATE tbl_conceptmetadata SET dispLocation = 148 WHERE localConceptId = 'cid:local:%2$s'", user, STORE_TITLE);
  run_info("INSERT INTO lineup_system VALUES ('NPXS40047','cid:local:NPXS40047',4,2);"
           "UPDATE tbl_appinfo SET val = 148 WHERE titleId = 'NPXS40047' AND key = 'DISPLAYLOCATION';"
           "UPDATE tbl_conceptinfo SET val = 148 WHERE local_concept_id = 'cid:local:NPXS40047' AND key = 'display_location'");
  assert(release_unmovable() == 1 && release_unmovable() == 0);
  expect_system(user, STORE_TITLE, 146, 2194);
  assert(!scalar("SELECT COUNT(*) FROM lineup_system WHERE titleId = 'NPXS40047'") &&
         scalar("SELECT COUNT(*) FROM lineup_system") == 1);
  assert(!info_scalar("SELECT COUNT(*) FROM lineup_system WHERE titleId = 'NPXS40047'"));

  // An app another tool half moved to Media (Game Compressor: its concept left on Games): to Games and back to Media
  // puts every place on Media, the concept ones too, and keeps it there.
  const char* gc = "PSGC50001";
  assert(move_app(gc, GAMES_TAB, backup, sizeof backup, message, sizeof message) == 200);
  assert(!scalar("SELECT COUNT(*) FROM sqlite_master WHERE name GLOB 'lineup_moved*'"));
  assert(move_app(gc, MEDIA_TAB, backup, sizeof backup, message, sizeof message) == 200);
  assert(scalar("SELECT COUNT(*) FROM lineup_moved WHERE titleId = 'PSGC50001' AND tab = 188 AND home = 138") == 1);
  const char* gc_concept = "SELECT COUNT(*) FROM tbl_concepticoninfo_%1$s WHERE localConceptId = 'cid:local:%2$s' AND "
                           "dispLocation = 2236 AND hubAppUri = 'psmediahub:main?titleId=%2$s'";
  const char* gc_meta = "SELECT COUNT(*) FROM tbl_conceptmetadata m, json_each(m.ConceptInfoJson, '$.field_list') e WHERE "
                        "m.localConceptId = 'cid:local:%2$s' AND m.dispLocation = 188 AND m.pprHubAppUri = "
                        "'psmediahub:main?titleId=%2$s' AND e.value ->> '$.key' = 'display_location' AND "
                        "e.value ->> '$.data' = 188";
  const char* gc_info = "SELECT group_concat(val, '|') FROM (SELECT val FROM tbl_conceptinfo WHERE local_concept_id = "
                        "'cid:local:PSGC50001' AND key IN ('display_location', 'hubapp_uri') ORDER BY key)";
  assert(scalar_of(gc_concept, user, gc) == 1 && scalar_of(gc_meta, user, gc) == 1);
  assert(scalar_of("SELECT categoryType & 65536 FROM tbl_contentinfo WHERE titleId = '%2$s'", user, gc));
  {
    sqlite3* info;
    sqlite3_stmt* statement;
    assert(sqlite3_open(appinfo_path, &info) == SQLITE_OK);
    assert(sqlite3_prepare_v2(info, gc_info, -1, &statement, NULL) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW);
    assert(!strcmp((const char*)sqlite3_column_text(statement, 0), "188|psmediahub:main?titleId=PSGC50001"));
    sqlite3_finalize(statement);
    sqlite3_close(info);
  }
  // Installed again as a game, as Game Compressor's payload does: every concept place comes back to Media too.
  run("UPDATE tbl_concepticoninfo_%1$s SET dispLocation = 1162, hubAppUri = 'pshome:gamehub?titleId=%2$s' WHERE "
      "localConceptId = 'cid:local:%2$s';"
      "UPDATE tbl_conceptmetadata SET dispLocation = 138 WHERE localConceptId = 'cid:local:%2$s'", user, gc);
  run_info("UPDATE tbl_conceptinfo SET val = 138 WHERE local_concept_id = 'cid:local:PSGC50001' AND key = "
           "'display_location'");
  assert(scalar_of(gc_concept, user, gc) == 1);
  assert(scalar_of("SELECT dispLocation FROM tbl_conceptmetadata WHERE localConceptId = 'cid:local:%2$s'", user, gc) == 188);
  assert(info_scalar("SELECT val FROM tbl_conceptinfo WHERE local_concept_id = 'cid:local:PSGC50001' AND key = "
                     "'display_location'") == 188);

  // Welcome: shown on every profile with the hidden bit cleared everywhere, kept shown when the system hides it at a
  // restart, and hidden again exactly as the system had it.
  expect_welcome(user, 147, 0);
  assert(show_welcome(0, backup, sizeof backup, message, sizeof message) == 409);
  assert(show_welcome(1, backup, sizeof backup, message, sizeof message) == 200);
  expect_welcome(user, 146, 1);
  assert(show_welcome(1, backup, sizeof backup, message, sizeof message) == 409);
  assert(move_system(WELCOME_TITLE, 1, backup, sizeof backup, message, sizeof message) == 400);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 0, dispLocation = 147 WHERE titleId = '%2$s';"
      "UPDATE tbl_contentinfo SET dispLocation = 147 WHERE titleId = '%2$s'", user, WELCOME_TITLE);
  run_info("UPDATE tbl_appinfo SET val = 147 WHERE titleId = 'NPXS40063' AND key = 'DISPLAYLOCATION'");
  assert(scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, WELCOME_TITLE) == 1);
  assert(scalar_of("SELECT dispLocation FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, WELCOME_TITLE) == 146);
  assert(scalar_of("SELECT dispLocation FROM tbl_contentinfo WHERE titleId = '%2$s'", user, WELCOME_TITLE) == 146);
  assert(info_scalar("SELECT val FROM tbl_appinfo WHERE titleId = 'NPXS40063' AND key = 'DISPLAYLOCATION'") == 146);
  run("UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', 147) WHERE titleId = '%2$s'",
      user, WELCOME_TITLE);
  assert(restore_system() >= 1 && restore_system() == 0);
  expect_welcome(user, 146, 1);
  assert(show_welcome(0, backup, sizeof backup, message, sizeof message) == 200);
  expect_welcome(user, 147, 0);
  assert(!scalar("SELECT COUNT(*) FROM lineup_system WHERE titleId = 'NPXS40063'"));

  // Every other system tile showing can hide, and stays hidden; the system's own hidden ones offer nothing.
  const char* plus = "NPXS40037";
  if (!scalar_of("SELECT COUNT(*) FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, plus))
    run("INSERT INTO tbl_iconinfo_%1$s (titleId, titleName, lastAccessIndex, visible, dispLocation) VALUES "
        "('%2$s','PlayStation Plus',0,0,147)", user, plus);
  assert(!scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, plus));
  assert(set_hidden(user, plus, 1, backup, sizeof backup, message, sizeof message) == 409);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE titleId = '%2$s'", user, plus);  // the system shows it
  assert(set_hidden(user, plus, 1, backup, sizeof backup, message, sizeof message) == 200);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 1 WHERE titleId = '%2$s'", user, plus);  // and again, at a restart
  assert(!scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, plus));
  assert(set_hidden(user, plus, 0, backup, sizeof backup, message, sizeof message) == 200);
  assert(scalar_of("SELECT visible FROM tbl_iconinfo_%1$s WHERE titleId = '%2$s'", user, plus) == 1);
  run("UPDATE tbl_iconinfo_%1$s SET visible = 0 WHERE titleId = '%2$s'", user, plus);  // as the system had it

  // The Media Gallery is on its tab's row, and pins there: kept first while another app is opened.
  {
    static Row rows[MAX_GAMES], posted[MAX_GAMES];
    int count = tab_rows(user, MEDIA_TAB, rows), at = -1;
    for (int i = 0; i < count; ++i) if (!strcmp(rows[i].title, GALLERY_TITLE)) at = i;
    assert(at >= 0 && count >= 2);
    posted[0] = rows[at];
    posted[0].pinned = 1;
    for (int i = 0, j = 1; i < count; ++i) if (i != at) posted[j++] = rows[i];
    char integrity[256];
    assert(save_order(user, MEDIA_TAB, posted, count, backup, sizeof backup, integrity, sizeof integrity, message,
                      sizeof message) == 200 && !strcmp(integrity, "ok"));
    assert(tab_rows(user, MEDIA_TAB, rows) == count && !strcmp(rows[0].title, GALLERY_TITLE));
    char other[16];
    snprintf(other, sizeof other, "%s", rows[count - 1].title);
    open_game(user, other);
    assert(tab_rows(user, MEDIA_TAB, rows) == count && !strcmp(rows[0].title, GALLERY_TITLE) &&
           !strcmp(rows[1].title, other));
    // Moved back to Games, it loses its pin and joins that row, and MyHome again in the place it left (last when it
    // was not there), the others in order from 1.
    assert(move_system(GALLERY_TITLE, 0, backup, sizeof backup, message, sizeof message) == 200);
    assert(tab_rows(user, MEDIA_TAB, rows) == count - 1);
    long long size = scalar_of(home_size, user, "");
    assert(size == home_count + !gallery_home);
    assert(scalar_of(home_place, user, GALLERY_TITLE) == (gallery_home ? gallery_home : size));
    assert(scalar_of("SELECT idx FROM tbl_group_%1$s WHERE groupName = 'MyHome' AND itemId = '%2$s'", user,
                     GALLERY_TITLE) == (gallery_home ? gallery_home : size));
    assert(!scalar_of("SELECT COUNT(*) FROM tbl_group_%1$s g WHERE g.groupName = 'MyHome' AND g.idx <> (SELECT COUNT(*) "
                      "FROM tbl_group_%1$s o WHERE o.groupName = 'MyHome' AND o.idx <= g.idx)", user, ""));
    assert(!scalar("SELECT COUNT(*) FROM sqlite_master WHERE name GLOB 'lineup_home_*'"));
    // On the synthetic databases, the whole group.
    assert(strcmp(user, "0000000007") ||
           scalar_of("SELECT group_concat(itemId, '|') = 'PPSA00001|NPXS40075|NPXS40071' FROM (SELECT itemId FROM "
                     "tbl_group_%1$s WHERE groupName = 'MyHome' ORDER BY idx)", user, ""));
    assert(!scalar_of("SELECT COUNT(*) FROM sqlite_master WHERE name = 'lineup_pin'", user, "") ||
           !scalar_of("SELECT COUNT(*) FROM lineup_pin WHERE titleId = '%2$s'", user, GALLERY_TITLE));
    count = tab_rows(user, GAMES_TAB, rows);
    at = -1;
    for (int i = 0; i < count; ++i) if (!strcmp(rows[i].title, GALLERY_TITLE)) at = i;
    assert(at >= 0);
    assert(move_system(GALLERY_TITLE, 1, backup, sizeof backup, message, sizeof message) == 200);
  }

  // TabShifter's tile goes from both databases, once; its nsx flags stay.
  long long flags = scalar_of("SELECT COUNT(*) FROM tbl_info_%1$s WHERE key GLOB 'nsx_content_flag_*'", user, "");
  int removed = 0, folders = 0;
  // Its folders go too, and only those that were there count.
  tabshifter_dirs[0] = "/tmp/lineupd-test-tbsh";
  tabshifter_dirs[1] = "/tmp/lineupd-test-tbsh-meta";
  tabshifter_dirs[2] = "/tmp/lineupd-test-tbsh-none";
  assert(!system("rm -rf /tmp/lineupd-test-tbsh* && mkdir -p /tmp/lineupd-test-tbsh/sce_sys /tmp/lineupd-test-tbsh-meta && "
                 "touch /tmp/lineupd-test-tbsh/sce_sys/param.json"));
  assert(remove_tabshifter(&removed, &folders, backup, sizeof backup, message, sizeof message) == 200 && removed > 0 &&
         folders == 2);
  assert(access("/tmp/lineupd-test-tbsh", F_OK) && access("/tmp/lineupd-test-tbsh-meta", F_OK));
  assert(!scalar("SELECT COUNT(*) FROM tbl_contentinfo WHERE titleId = 'TBSH00001'"));
  assert(!scalar_of("SELECT COUNT(*) FROM tbl_iconinfo_%1$s WHERE titleId = 'TBSH00001'", user, ""));
  assert(!scalar_of("SELECT COUNT(*) FROM tbl_concepticoninfo_%1$s WHERE localConceptId = 'cid:local:TBSH00001'", user, ""));
  assert(!info_scalar("SELECT COUNT(*) FROM tbl_appinfo WHERE titleId = 'TBSH00001'") &&
         !info_scalar("SELECT COUNT(*) FROM tbl_conceptinfo WHERE local_concept_id = 'cid:local:TBSH00001'"));
  assert(scalar_of("SELECT COUNT(*) FROM tbl_info_%1$s WHERE key GLOB 'nsx_content_flag_*'", user, "") == flags);
  assert(remove_tabshifter(&removed, &folders, backup, sizeof backup, message, sizeof message) == 200 && !removed &&
         !folders);
  // No rows left but a folder: the folder goes and counts.
  assert(!mkdir("/tmp/lineupd-test-tbsh-meta", 0755));
  assert(remove_tabshifter(&removed, &folders, backup, sizeof backup, message, sizeof message) == 200 && !removed &&
         folders == 1);

  // /pins/uninstall takes every lineup_* object out of both databases and leaves the tiles where they are.
  assert(integrity_ok(db_path) && integrity_ok(appinfo_path));
  assert(unpin(NULL, backup, sizeof backup, message, sizeof message) == 200);
  assert(!scalar("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'lineup%'"));
  assert(!info_scalar("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'lineup%'"));
  expect_system(user, GALLERY_TITLE, 156, 2204);
  assert(integrity_ok(db_path) && integrity_ok(appinfo_path));
}

// Self-check: lineupd-test [<copy of app.db> <userId> [<copy of appinfo.db>]]. Reorders the copy (or a synthetic
// database), checks it, restores it. Given a copy of appinfo.db as well, moves the system tiles on the copies.
int main(int argc, char** argv) {
  Row current[3] = {{"PPSA00001", 30, 0}, {"PPSA00002", 20, 0}, {"PPSA00003", 10, 0}};
  Row posted[3] = {{"PPSA00003", 10, 0}, {"PPSA00001", 30, 0}, {"PPSA00002", 20, 0}};
  long long out[3];
  assert(!plan_reorder(current, 3, posted, 3, out) && out[0] == 30 && out[1] == 20 && out[2] == 10);
  // Renumbered by the console in the same order, the indices read still save; read in another order, they do not.
  Row renumbered[3] = {{"PPSA00003", 11, 0}, {"PPSA00001", 33, 0}, {"PPSA00002", 21, 0}};
  assert(!plan_reorder(current, 3, renumbered, 3, out) && out[0] == 30 && out[1] == 20 && out[2] == 10);
  Row stale[3] = {{"PPSA00003", 40, 0}, {"PPSA00001", 30, 0}, {"PPSA00002", 20, 0}};
  assert(plan_reorder(current, 3, stale, 3, out));
  Row duplicate[3] = {{"PPSA00001", 30, 0}, {"PPSA00001", 30, 0}, {"PPSA00002", 20, 0}};
  assert(plan_reorder(current, 3, duplicate, 3, out));
  assert(plan_reorder(current, 3, posted, 2, out));
  Row parsed[4];
  assert(parse_order("LIN1\nPPSA00003 10\nPPSA00001 30\n", parsed, 4) == 2 && parsed[1].index == 30);
  assert(parse_order("LIN1\nPPSA0003 10\n", parsed, 4) < 0 && parse_order("XXX\n", parsed, 4) < 0);
  assert(parse_order("LIN1\nPPSA00003 10 x\n", parsed, 4) < 0 && parse_order("LIN1\nPPSA00003 10 pin x\n", parsed, 4) < 0);
  assert(parse_order("LIN1\nPPSA00003 10 pin\nPPSA00001 30\n", parsed, 4) == 2 && parsed[0].pinned && !parsed[1].pinned);

  // Without a copy of app.db, a synthetic one: five games on the row and one hidden.
  const char* user = argc >= 3 ? argv[2] : "0000000001";
  db_path = argc >= 3 ? argv[1] : "/tmp/lineupd-test.db";
  backup_dir = "/tmp/lineupd-test-backups";
  appinfo_path = argc >= 4 ? argv[3] : "/tmp/lineupd-test-appinfo.db";
  sqlite3* db;
  if (argc < 3) {
    unlink(db_path);
    assert(sqlite3_open(db_path, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE tbl_iconinfo_0000000001 (titleId TEXT, titleName TEXT, lastAccessIndex INTEGER, "
                            "visible INTEGER, dispLocation INTEGER);"
                            "INSERT INTO tbl_iconinfo_0000000001 VALUES ('PPSA00001','One',50,1,138),('PPSA00002','Two',40,1,138),"
                            "('PPSA00003','Three',30,1,138),('PPSA00004','Four',20,1,138),('PPSA00005','Five',10,1,138),"
                            "('PPSA00006','Hidden',60,1,188);", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
  }
  static Row before[MAX_GAMES], after[MAX_GAMES], order[MAX_GAMES];
  db = open_db(0);
  int count = read_tab(db, user, GAMES_TAB, before, NULL, MAX_GAMES);
  sqlite3_close(db);
  assert(count >= 3);
  // Move the third game to the front.
  order[0] = before[2];
  for (int i = 0, j = 1; i < count; ++i) if (i != 2) order[j++] = before[i];
  char backup[256], integrity[256], message[300];
  int status = save_order(user, GAMES_TAB, order, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message);
  if (status != 200) { fprintf(stderr, "%d %s\n", status, message); return 1; }
  assert(!strcmp(integrity, "ok"));
  db = open_db(0);
  assert(read_tab(db, user, GAMES_TAB, after, NULL, MAX_GAMES) == count);
  sqlite3_close(db);
  for (int i = 0; i < count; ++i) assert(!strcmp(after[i].title, order[i].title) && after[i].index == before[i].index);
  // A stale save is refused.
  assert(save_order(user, GAMES_TAB, order, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message) == 409);
  // Restore the original order.
  for (int i = 0; i < count; ++i) { order[i] = before[i]; for (int j = 0; j < count; ++j) if (!strcmp(after[j].title, before[i].title)) order[i].index = after[j].index; }
  assert(save_order(user, GAMES_TAB, order, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message) == 200);
  db = open_db(0);
  read_tab(db, user, GAMES_TAB, after, NULL, MAX_GAMES);
  sqlite3_close(db);
  for (int i = 0; i < count; ++i) assert(!strcmp(after[i].title, before[i].title) && after[i].index == before[i].index);
  if (argc < 3) test_pins();
  // System tiles: on the copies given, else on a synthetic pair.
  if (argc >= 4) test_system(user);
  else if (argc < 3) {
    const char *app = db_path, *info = appinfo_path;
    db_path = "/tmp/lineupd-test-system.db";
    appinfo_path = "/tmp/lineupd-test-system-appinfo.db";
    make_system_databases("0000000007");
    test_system("0000000007");
    db_path = app;
    appinfo_path = info;
  }

  // AppInfoJson in both shapes: the field list's entries change (and a top-level copy an earlier Lineup added goes),
  // a flat one gets its keys; APP_FIELD reads either.
  {
    sqlite3* j;
    assert(sqlite3_open(":memory:", &j) == SQLITE_OK);
    assert(sqlite3_exec(j, "CREATE TABLE tbl_contentinfo (titleId TEXT, AppInfoJson TEXT);"
                           "INSERT INTO tbl_contentinfo VALUES ('FLAT00001', '{\"DISPLAYLOCATION\":138,\"TITLE\":\"a\"}'),"
                           "('LIST00001', '{\"field_list\":[{\"data\":138,\"key\":\"DISPLAYLOCATION\",\"size\":8,\"type\":0},"
                           "{\"data\":\"pshome:gamehub?titleId=LIST00001\",\"key\":\"HUBAPP_URI\",\"size\":32,\"type\":2}],"
                           "\"mimeType\":0,\"DISPLAYLOCATION\":138}'), ('BAD000001', '{');", NULL, NULL, NULL) == SQLITE_OK);
    const char* hub = "psmediahub:main?titleId=LIST00001";
    for (int i = 0; i < 3; ++i) {
      const char* t = i == 0 ? "FLAT00001" : i == 1 ? "LIST00001" : "BAD000001";
      assert(!set_app_field(j, t, "DISPLAYLOCATION", 188, NULL) && !set_app_field(j, t, "HUBAPP_URI", 0, hub));
    }
    sqlite3_stmt* s;
    assert(sqlite3_prepare_v2(j, "SELECT titleId, " APP_FIELD("AppInfoJson", "DISPLAYLOCATION") ", "
                              APP_FIELD("AppInfoJson", "HUBAPP_URI") ", AppInfoJson FROM tbl_contentinfo ORDER BY rowid",
                              -1, &s, NULL) == SQLITE_OK);
    for (int i = 0; i < 3; ++i) {
      assert(sqlite3_step(s) == SQLITE_ROW);
      const char* json = (const char*)sqlite3_column_text(s, 3);
      if (i == 2) { assert(!strcmp(json, "{") && sqlite3_column_type(s, 1) == SQLITE_NULL); continue; }
      assert(sqlite3_column_int(s, 1) == 188 && !strcmp((const char*)sqlite3_column_text(s, 2), hub));
      if (i == 1)
        assert(strstr(json, "\"size\":33") && strstr(json, "\"mimeType\":0}") && !strstr(json, "\"HUBAPP_URI\":"));
      else assert(strstr(json, "\"TITLE\":\"a\""));
    }
    sqlite3_finalize(s);
    sqlite3_close(j);
  }

  // Profile pictures: folders named with 8 and 16 hex digits.
  profile_cache_dir = "/tmp/lineupd-test-profile";
  assert(!system("rm -rf /tmp/lineupd-test-profile && mkdir -p /tmp/lineupd-test-profile/0x12F0D5AE "
                 "/tmp/lineupd-test-profile/0x0000000112F0D5AF /tmp/lineupd-test-profile/other"));
  char avatar[512];
  assert(!avatar_path("0317773230", avatar, sizeof avatar) && !strcmp(avatar, "/tmp/lineupd-test-profile/0x12F0D5AE/avatar.png"));
  assert(!avatar_path("0317773231", avatar, sizeof avatar) && strstr(avatar, "/0x0000000112F0D5AF/avatar.png"));
  assert(avatar_path("0000000005", avatar, sizeof avatar));

  // Updates: versions, a staged app folder, and a swap that adds, replaces and removes files.
  assert(version_number("1.2.0") == 1002000 && version_number("01.002.000") == 1002000);
  assert(version_number("1.2") < 0 && version_number("1.2.0.") < 0 && version_number(" 1.2.0") < 0 && version_number("previous") < 0);
  assert(!system("rm -rf /tmp/lineupd-test-update && mkdir -p /tmp/lineupd-test-update/new/sce_sys "
                 "/tmp/lineupd-test-update/new/fonts /tmp/lineupd-test-update/cur/old && cd /tmp/lineupd-test-update && "
                 "printf '{\\n  \"contentVersion\": \"01.002.000\",\\n  \"titleId\": \"PPSA99077\"\\n}\\n' > new/sce_sys/param.json && "
                 "echo new > new/eboot.bin && echo d > new/lineupd.elf && echo f > new/fonts/a.ttf && "
                 "echo old > cur/eboot.bin && echo gone > cur/gone.txt && echo x > cur/old/x && echo x > cur/fonts"));
  assert(app_folder_version("/tmp/lineupd-test-update/new") == 1002000 && app_folder_version("/tmp/lineupd-test-update/cur") < 0);
  assert(!copy_tree("/tmp/lineupd-test-update/cur", "/tmp/lineupd-test-update/saved"));
  assert(!swap_tree("/tmp/lineupd-test-update/new", "/tmp/lineupd-test-update/cur"));
  assert(!system("cd /tmp/lineupd-test-update && diff -r new cur && [ \"$(cat saved/eboot.bin)\" = old ] && "
                 "[ -f saved/old/x ] && [ -z \"$(ls -A cur | grep '\\.new$')\" ]"));
  assert(!remove_tree("/tmp/lineupd-test-update") && !remove_tree("/tmp/lineupd-test-update"));
  printf("logic ok, db round trip ok (%d games, backup %s)\n", count, backup);
  return 0;
}

#endif
