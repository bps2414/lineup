// SPDX-License-Identifier: GPL-3.0-or-later
//
// lineupd: a small HTTP service that reads the PS5 home-screen game row and saves a new order.
// The home row is the rows of /system_data/priv/mms/app.db table tbl_iconinfo_<userId> with
// visible = 1 and dispLocation = 138, shown by lastAccessIndex, largest first. Saving gives the
// posted order the indices those rows already hold, so no index grows past the counter the home
// screen keeps in memory. Writes go through SQLite in place, inside one transaction, after a backup.
//
// Endpoints (JSON unless noted), on 127.0.0.1:9077 (0.0.0.0 when built with -DLINEUPD_LAN):
//   GET  /version                {"version"}
//   GET  /profiles                 {"profiles":[{"userId","hex","name","avatar"}]}
//   GET  /row?user=<10 dig>  {"user","pins":"on"|"missing"|"off","games":[{"titleId","name","index","icon",
//                                "pinned":slot|null}]}
//   GET  /icon/<titleId>         image/png
//   GET  /avatar/<10 dig>        image/png, the profile's picture
//   POST /row?user=<10 dig>  X-Lineup: 1; body "LIN1\n" then "<titleId> <index>[ pin]\n" per game,
//                                in the new order, with the index the client read; " pin" pins it at that place
//   POST /pins/off?user=<10 dig> X-Lineup: 1; unpins that profile's games
//   POST /pins/uninstall         X-Lineup: 1; removes every pin, trigger and table Lineup added to app.db
//   POST /quit                   X-Lineup: 1; exits after answering
//   GET  /update/install         X-Lineup: 1; {"kind":"folder"|"image","path","previous"}
//   POST /update/apply?version=<x.y.z|previous>  X-Lineup: 1; answers, waits for the app to close, swaps its
//                                files for the downloaded ones (or the previous ones) and opens it again
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

#define LINEUPD_VERSION "1.2.0"
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
#define KEEP_BACKUPS 5

static const char* db_path = "/system_data/priv/mms/app.db";
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
// posted order. Fails when the posted list is not exactly the current rows with the indices read.
static int plan_reorder(const Row* current, int count, const Row* posted, int posted_count, long long* out) {
  if (count != posted_count || count <= 0) return -1;
  for (int i = 0; i < posted_count; ++i) {
    int found = 0;
    for (int j = 0; j < count; ++j)
      if (!strcmp(posted[i].title, current[j].title)) { found = current[j].index == posted[i].index; break; }
    if (!found) return -1;
    for (int j = 0; j < i; ++j) if (!strcmp(posted[i].title, posted[j].title)) return -1;
  }
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

// `user` must already be validated: table names cannot be bound.
static int read_row(sqlite3* db, const char* user, Row* rows, char (*names)[256], int capacity) {
  char sql[160];
  snprintf(sql, sizeof sql, "SELECT titleId, titleName, lastAccessIndex FROM tbl_iconinfo_%s "
           "WHERE visible = 1 AND dispLocation = 138 ORDER BY lastAccessIndex DESC", user);
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

// Keeps the newest KEEP_BACKUPS app-*.db files; names carry a zero-padded time, so they sort by age.
static void rotate_backups(void) {
  DIR* dir = opendir(backup_dir);
  if (!dir) return;
  char* names[256];
  int count = 0;
  for (struct dirent* entry; (entry = readdir(dir)) && count < 256;)
    if (!strncmp(entry->d_name, "app-", 4) && strstr(entry->d_name, ".db")) names[count++] = strdup(entry->d_name);
  closedir(dir);
  qsort(names, (size_t)count, sizeof *names, names_ascending);
  for (int i = 0; i < count; ++i) {
    if (i < count - KEEP_BACKUPS) {
      char path[512];
      snprintf(path, sizeof path, "%s/%s", backup_dir, names[i]);
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
  snprintf(backup, backup_size, "%s/app-%010lld.db", backup_dir, (long long)time(NULL));
  unlink(backup);
  char sql[600];
  snprintf(sql, sizeof sql, "VACUUM INTO '%s'", backup);
  if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The backup failed (%s). Nothing was saved.", sqlite3_errmsg(db));
    return -1;
  }
  rotate_backups();
  return 0;
}

// ---- Pinned games ----
//
// A pinned game keeps its place when the system moves games: three triggers on tbl_iconinfo_<user>
// (lineup_pin_u_, _i_ and _d_<user>) run REORDER inside the system's own write, so the home screen rereads a row
// already in the pinned order. lineup_pin holds the slots, lineup_seq is 0..MAX_GAMES-1 (to find the k-th free
// place) and lineup_tmp is REORDER's scratch table, empty between writes.

#define PINNED "SELECT titleId FROM lineup_pin WHERE user = '%1$s'"

// In SQL the home screen's SQLite reads: no CTEs (not allowed in triggers anyway), window functions, UPSERT or
// RETURNING. %1$s is the user. In lineup_tmp, v is a game's index, r the place it gets and nv that place's index
// (before that, for a game that is not pinned, how many of those were used after it).
// ponytail: places stop at MAX_GAMES; a game past them gets no place and keeps its index.
#define REORDER \
  /* The row as it is now. */ \
  "INSERT OR IGNORE INTO lineup_tmp (titleId, v) SELECT titleId, lastAccessIndex FROM tbl_iconinfo_%1$s " \
  "WHERE visible = 1 AND dispLocation = 138;" \
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
  "tbl_iconinfo_%1$s.titleId) WHERE visible = 1 AND dispLocation = 138 AND titleId IN " \
  "(SELECT titleId FROM lineup_tmp WHERE nv <> v);" \
  "DELETE FROM lineup_tmp;"

// Opening a game raises its index (twice, a few seconds apart); installing inserts and uninstalling deletes a row.
// recursive_triggers is off by default, so REORDER's own UPDATE does not fire them again; a non-empty lineup_tmp
// stops them too, should it be on.
#define PIN_TRIGGER(kind, event, when) \
  "CREATE TRIGGER lineup_pin_" kind "_%1$s AFTER " event " ON tbl_iconinfo_%1$s WHEN " when \
  "NOT EXISTS (SELECT 1 FROM lineup_tmp) BEGIN " REORDER " END;"
#define PIN_TRIGGERS \
  PIN_TRIGGER("u", "UPDATE OF lastAccessIndex", "NEW.lastAccessIndex > OLD.lastAccessIndex AND ") \
  PIN_TRIGGER("i", "INSERT", "") PIN_TRIGGER("d", "DELETE", "")

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

// Inside save_order's transaction, after its triggers were dropped: records the posted games that are pinned, at
// their posted places, and creates the triggers again when there are any.
static int save_pins(sqlite3* db, const char* user, const Row* posted, int count) {
  int pins = 0;
  for (int i = 0; i < count; ++i) pins += posted[i].pinned;
  if (forget_pins(db, user)) return -1;
  if (!pins) return remove_pins_if_unused(db);
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
  static char sql[16 * 1024];
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

// Unpins every game of `user` (/pins/off), or of every profile when `user` is NULL (/pins/uninstall), after a
// backup. Returns the HTTP status; `message` explains a failure.
static int unpin(const char* user, char* backup, size_t backup_size, char* message, size_t message_size) {
  sqlite3* db = open_db(1);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return 500; }
  int status = 500;
  if (user && !table_exists(db, user)) { snprintf(message, message_size, "No such profile."); status = 404; goto done; }
  // Nothing to remove: no backup either.
  if (user && !has_object(db, "lineup_pin")) { status = 200; goto done; }
  if (check_and_backup(db, backup, backup_size, message, message_size)) goto done;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
    snprintf(message, message_size, "The database is busy (%s). Try again.", sqlite3_errmsg(db));
    goto done;
  }
  if ((user ? drop_pin_triggers(db, user) || forget_pins(db, user) || remove_pins_if_unused(db) : remove_pins(db)) ||
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

// Saves `posted` as the new order. Returns the HTTP status; `message` explains a failure.
static int save_order(const char* user, const Row* posted, int posted_count, char* backup, size_t backup_size,
                      char* integrity, size_t integrity_size, char* message, size_t message_size) {
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
  int count = read_row(db, user, current, NULL, MAX_GAMES);
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
    if (!failed) failed = save_pins(db, user, posted, posted_count);
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

// Copies a file to `to`, created or truncated, and flushes it to the disk.
static int copy_file(const char* from, const char* to) {
  static char chunk[64 * 1024];
  int in = open(from, O_RDONLY), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  int failed = out < 0;
  for (ssize_t count; !failed && (count = read(in, chunk, sizeof chunk)) != 0;)
    failed = count < 0 || write(out, chunk, (size_t)count) != count;
  // The console's own services read a title's files, so they are open to every user.
  if (!failed) failed = fchmod(out, 0777) || fsync(out);
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
// as .<name>.new, flushed and renamed over it; then what `from` lacks is removed. `to` stays the same directory,
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

static void get_row(int socket, const char* user) {
  static Row rows[MAX_GAMES];
  static char names[MAX_GAMES][256];
  sqlite3* db = open_db(0);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  if (!table_exists(db, user)) { sqlite3_close(db); respond_error(socket, 404, "No such profile."); return; }
  int count = read_row(db, user, rows, names, MAX_GAMES);
  static long long icons[MAX_GAMES];
  static int slots[MAX_GAMES];
  for (int i = 0; i < count; ++i) {
    char path[256];
    struct stat info;
    icon_path(db, rows[i].title, path, sizeof path);
    icons[i] = stat(path, &info) ? 0 : (long long)info.st_mtime;
  }
  const char* pins = read_pins(db, user, rows, count < 0 ? 0 : count, slots);
  sqlite3_close(db);
  if (count < 0) { respond_error(socket, 500, "Could not read the row."); return; }
  Buffer body = {0};
  puts_(&body, "{\"user\":");
  put_json_string(&body, user);
  putf(&body, ",\"pins\":\"%s\",\"games\":[", pins);
  for (int i = 0; i < count; ++i) {
    puts_(&body, i ? ",{\"titleId\":" : "{\"titleId\":");
    put_json_string(&body, rows[i].title);
    puts_(&body, ",\"name\":");
    put_json_string(&body, names[i]);
    putf(&body, ",\"index\":%lld,\"icon\":%lld", rows[i].index, icons[i]);
    if (slots[i] < 0) puts_(&body, ",\"pinned\":null}");
    else putf(&body, ",\"pinned\":%d}", slots[i]);
  }
  puts_(&body, "]}");
  respond_json(socket, 200, &body);
  free(body.data);
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

static void post_row(int socket, const char* user, const char* body) {
  static Row posted[MAX_GAMES];
  int count = parse_order(body, posted, MAX_GAMES);
  if (count <= 0) { respond_error(socket, 400, "The posted order is malformed."); return; }
  char backup[256] = "", integrity[256] = "", message[300] = "";
  int status = save_order(user, posted, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message);
  if (status != 200) { respond_error(socket, status, message); return; }
  LOG("saved %d games for %s, backup %s, integrity %s", count, user, backup, integrity);
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
  update.pending = 1;
  update.rollback = rollback;
  update.install = install;
  snprintf(update.source, sizeof update.source, "%s", source);
  LOG("update to %s from %s, %s", version, source, install.path);
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

// Swaps the installed files for update.source. The installed version goes to update_dir/previous first; when
// rolling back, previous/ itself is the source, so it moves aside to rollback/ until the swap is done.
static void replace_files(void) {
  char previous[256], rollback[256], source[512], saved[512];
  snprintf(previous, sizeof previous, "%s/previous", update_dir);
  snprintf(rollback, sizeof rollback, "%s/rollback", update_dir);
  snprintf(source, sizeof source, "%s", update.source);
  const char* installed = update.install.path;
  if (update.rollback) {
    remove_tree(rollback);
    if (rename(previous, rollback)) { update_failed("Could not move the previous version aside (%s). Nothing changed.", strerror(errno)); return; }
    snprintf(source, sizeof source, "%s/%s", rollback, strrchr(update.source, '/') + 1);
  }
  int failed = remove_tree(previous);
  make_dirs(previous);
  snprintf(saved, sizeof saved, "%s/%s", previous, strrchr(installed, '/') + 1);
  if (failed) update_failed("Could not clear %s (%s). Nothing changed.", previous, strerror(errno));
  else if (update.install.image) {
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
  } else if ((failed = copy_tree(installed, saved))) {
    update_failed("Could not copy the installed version to %s (%s). Nothing changed.", saved, strerror(errno));
  } else if ((failed = swap_tree(source, installed))) {
    int restored = !swap_tree(saved, installed);
    update_failed("Could not replace the files (%s). %s", strerror(errno),
                  restored ? "The installed version was put back." : "Copy the app folder again by FTP.");
  }
  if (update.rollback && failed) { remove_tree(previous); rename(rollback, previous); }
  else if (update.rollback) remove_tree(rollback);
}

// What relauncher.c does: wait for the app to close, then open it again for the foreground user.
static void run_update(void) {
  update.pending = 0;
  char error[256];
  snprintf(error, sizeof error, "%s/error", update_dir);
  unlink(error);
  for (int waited = 0; sceLncUtilGetAppId(TITLE_ID) >= 0; waited += 100) {
    if (waited >= 30000) { update_failed("Lineup did not close within 30 seconds. Nothing changed."); return; }
    usleep(100 * 1000);
  }
  usleep(500 * 1000);
  replace_files();
  sceUserServiceInitialize(NULL);
  LncAppParam param = {sizeof param, 0, 0, 0, 0};
  sceUserServiceGetForegroundUser((int*)&param.user_id);
  LOG("launch %s: %d", TITLE_ID, sceLncUtilLaunchApp(TITLE_ID, NULL, &param));
}

#ifdef LINEUPD_DIAG

// ---- Phase 0 diagnostics ----

static void diag_error(Buffer* b, const char* message) {
  puts_(b, "{\"error\":");
  put_json_string(b, message);
  puts_(b, "}");
}

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

// Each result row as an object keyed by column name.
static void diag_query(Buffer* b, sqlite3* db, const char* sql) {
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) { diag_error(b, sqlite3_errmsg(db)); return; }
  puts_(b, "[");
  for (int row = 0; sqlite3_step(statement) == SQLITE_ROW; ++row) {
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
  sqlite3_finalize(statement);
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
    puts_(&reply, "{\"version\":\"" LINEUPD_VERSION "\"}");
    respond_json(socket, 200, &reply);
    free(reply.data);
  } else if (get && !strcmp(target, "/profiles")) {
    get_profiles(socket);
  } else if (!strcmp(target, "/row") && (get || post)) {
    if (!query_value(query, "user", user, sizeof user) || !is_user_id(user))
      respond_error(socket, 400, "user must be 10 digits.");
    else if (get) get_row(socket, user);
    else if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
    else post_row(socket, user, body);
  } else if (post && !strncmp(target, "/pins/", 6)) {
    if (!authorized) respond_error(socket, 403, "Missing the X-Lineup header.");
    else if (!strcmp(target, "/pins/uninstall")) post_unpin(socket, NULL);
    else if (strcmp(target, "/pins/off")) respond_error(socket, 404, "Unknown route.");
    else if (!query_value(query, "user", user, sizeof user) || !is_user_id(user)) respond_error(socket, 400, "user must be 10 digits.");
    else post_unpin(socket, user);
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

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  // The loader hands over its connection as stdio; let it go so the service outlives the sender.
  int null = open("/dev/null", O_RDWR);
  if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }

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
    return 0;
  }
  LOG("version %s listening on %d", LINEUPD_VERSION, PORT);
  for (int quit = 0; !quit;) {
    int client = accept(server, NULL, NULL);
    if (client < 0) { if (errno == EINTR) continue; break; }
    struct timeval timeout = {5, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    quit = handle(client);
    close(client);
    // After the answer is out: the app closes once it has it.
    if (update.pending) run_update();
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
  char sql[512];
  snprintf(sql, sizeof sql, format, user, title);
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

// Checks the row against `expected`, the games' last digits from first to last, and that no index went past
// the largest the system gave.
static void expect_row(const char* user, const char* expected) {
  static Row rows[MAX_GAMES];
  sqlite3* db = open_db(0);
  int count = read_row(db, user, rows, NULL, MAX_GAMES);
  sqlite3_close(db);
  char got[MAX_GAMES + 1];
  for (int i = 0; i < count; ++i) got[i] = rows[i].title[8];
  got[count] = 0;
  if (strcmp(got, expected)) { fprintf(stderr, "row %s, expected %s\n", got, expected); exit(1); }
  for (int i = 1; i < count; ++i) assert(rows[i].index < rows[i - 1].index);
  assert(rows[0].index <= given);
}

// Saves the row in `order` (last digits), pinning the games in `pins` at their places.
static void save_digits(const char* user, const char* order, const char* pins) {
  static Row current[MAX_GAMES], posted[MAX_GAMES];
  sqlite3* db = open_db(0);
  int count = read_row(db, user, current, NULL, MAX_GAMES);
  sqlite3_close(db);
  assert(count == (int)strlen(order));
  for (int i = 0; i < count; ++i)
    for (int j = 0; j < count; ++j)
      if (current[j].title[8] == order[i]) { posted[i] = current[j]; posted[i].pinned = strchr(pins, order[i]) != NULL; }
  char backup[256], integrity[256], message[300];
  int status = save_order(user, posted, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message);
  if (status != 200) { fprintf(stderr, "%d %s\n", status, message); exit(1); }
}

static const char* pins_of(const char* user, int* slots) {
  static Row rows[MAX_GAMES];
  sqlite3* db = open_db(0);
  int count = read_row(db, user, rows, NULL, MAX_GAMES);
  const char* state = read_pins(db, user, rows, count, slots);
  sqlite3_close(db);
  return state;
}

// Pinned games on the synthetic database: games 1 to 5 of user 1, in that order.
static void test_pins(void) {
  const char* one = "0000000001";
  int slots[MAX_GAMES];
  given = scalar("SELECT MAX(lastAccessIndex) FROM tbl_iconinfo_0000000001");
  save_digits(one, "31254", "35");
  expect_row(one, "31254");
  assert(!strcmp(pins_of(one, slots), "on") && slots[0] == 0 && slots[1] < 0 && slots[3] == 3);
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
  snprintf(sql, sizeof sql, "INSERT INTO tbl_iconinfo_%%1$s VALUES ('%%2$s','Seven',%lld,1,138)", given);
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
  // Without its triggers (a rebuilt database), the pins show as missing.
  run("DROP TRIGGER lineup_pin_d_%1$s", one, "");
  assert(!strcmp(pins_of(one, slots), "missing"));

  // /pins/off: the profile's pins and, the last ones gone, every lineup_* object; the system's order is back.
  char backup[256], message[300];
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
  assert(unpin(NULL, backup, sizeof backup, message, sizeof message) == 200);
  assert(!strcmp(pins_of(one, slots), "off") && !strcmp(pins_of(two, slots), "off"));
  assert(!scalar("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'lineup%'"));
}

// Self-check: lineupd-test [<copy of app.db> <userId>]. Reorders the copy (or a synthetic database), checks it,
// restores it.
int main(int argc, char** argv) {
  Row current[3] = {{"PPSA00001", 30, 0}, {"PPSA00002", 20, 0}, {"PPSA00003", 10, 0}};
  Row posted[3] = {{"PPSA00003", 10, 0}, {"PPSA00001", 30, 0}, {"PPSA00002", 20, 0}};
  long long out[3];
  assert(!plan_reorder(current, 3, posted, 3, out) && out[0] == 30 && out[1] == 20 && out[2] == 10);
  Row stale[3] = {{"PPSA00003", 11, 0}, {"PPSA00001", 30, 0}, {"PPSA00002", 20, 0}};
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
  int count = read_row(db, user, before, NULL, MAX_GAMES);
  sqlite3_close(db);
  assert(count >= 3);
  // Move the third game to the front.
  order[0] = before[2];
  for (int i = 0, j = 1; i < count; ++i) if (i != 2) order[j++] = before[i];
  char backup[256], integrity[256], message[300];
  int status = save_order(user, order, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message);
  if (status != 200) { fprintf(stderr, "%d %s\n", status, message); return 1; }
  assert(!strcmp(integrity, "ok"));
  db = open_db(0);
  assert(read_row(db, user, after, NULL, MAX_GAMES) == count);
  sqlite3_close(db);
  for (int i = 0; i < count; ++i) assert(!strcmp(after[i].title, order[i].title) && after[i].index == before[i].index);
  // A stale save is refused.
  assert(save_order(user, order, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message) == 409);
  // Restore the original order.
  for (int i = 0; i < count; ++i) { order[i] = before[i]; for (int j = 0; j < count; ++j) if (!strcmp(after[j].title, before[i].title)) order[i].index = after[j].index; }
  assert(save_order(user, order, count, backup, sizeof backup, integrity, sizeof integrity, message, sizeof message) == 200);
  db = open_db(0);
  read_row(db, user, after, NULL, MAX_GAMES);
  sqlite3_close(db);
  for (int i = 0; i < count; ++i) assert(!strcmp(after[i].title, before[i].title) && after[i].index == before[i].index);
  if (argc < 3) test_pins();

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
