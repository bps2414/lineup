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
//   GET  /profiles                 {"profiles":[{"userId","hex","name"}]}
//   GET  /row?user=<10 dig>  {"user","games":[{"titleId","name","index","icon"}]}
//   GET  /icon/<titleId>         image/png
//   POST /row?user=<10 dig>  X-Lineup: 1; body "LIN1\n" then "<titleId> <index>\n" per game,
//                                in the new order, with the index the client read
//   POST /quit                   X-Lineup: 1; exits after answering
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

#ifdef LINEUPD_TEST
#include <assert.h>
#define LOG(...) (fprintf(stderr, "[lineupd] " __VA_ARGS__), fputc('\n', stderr))
#else
#include <ps5/klog.h>
#define LOG(fmt, ...) klog_printf("[lineupd] " fmt "\n", ##__VA_ARGS__)
#endif

#define LINEUPD_VERSION "1.1.0"
#define PORT 9077
#define MAX_HEADER (16 * 1024)
#define MAX_BODY (64 * 1024)
#define MAX_GAMES 512
#define KEEP_BACKUPS 5

static const char* db_path = "/system_data/priv/mms/app.db";
static const char* backup_dir = "/data/lineup/backups";
static const char* home_dir = "/user/home";
static const char* appmeta_dir = "/user/appmeta";

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

typedef struct { char title[16]; long long index; } Row;

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

// Parses "LIN1\n" + "<titleId> <index>\n" lines. Returns the number of rows, or -1.
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
      char title[16];
      long long index;
      char extra;
      if (sscanf(text, "%15s %lld %c", title, &index, &extra) != 2 || !is_title_id(title)) return -1;
      strcpy(rows[count].title, title);
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

static int table_exists(sqlite3* db, const char* user) {
  char name[64];
  snprintf(name, sizeof name, "tbl_iconinfo_%s", user);
  sqlite3_stmt* statement;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &statement, NULL)) return 0;
  sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC);
  int exists = sqlite3_step(statement) == SQLITE_ROW;
  sqlite3_finalize(statement);
  return exists;
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

// Saves `posted` as the new order. Returns the HTTP status; `message` explains a failure.
static int save_order(const char* user, const Row* posted, int posted_count, char* backup, size_t backup_size,
                      char* integrity, size_t integrity_size, char* message, size_t message_size) {
  sqlite3* db = open_db(1);
  if (!db) { snprintf(message, message_size, "Could not open the home screen database."); return 500; }
  int status = 500;
  char check[128];
  static Row current[MAX_GAMES];
  static long long indices[MAX_GAMES];
  if (!table_exists(db, user)) { snprintf(message, message_size, "No such profile."); status = 404; goto done; }
  if (pragma_text(db, "PRAGMA quick_check", check, sizeof check)) {
    snprintf(message, message_size, "The home screen database failed its check (%s). Nothing was saved.", check);
    goto done;
  }
  make_dirs(backup_dir);
  snprintf(backup, backup_size, "%s/app-%010lld.db", backup_dir, (long long)time(NULL));
  unlink(backup);
  {
    char sql[600];
    snprintf(sql, sizeof sql, "VACUUM INTO '%s'", backup);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
      snprintf(message, message_size, "The backup failed (%s). Nothing was saved.", sqlite3_errmsg(db));
      goto done;
    }
  }
  rotate_backups();
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
    sqlite3_stmt* update;
    int failed = sqlite3_prepare_v2(db, sql, -1, &update, NULL) != SQLITE_OK;
    for (int i = 0; !failed && i < posted_count; ++i) {
      sqlite3_bind_int64(update, 1, indices[i]);
      sqlite3_bind_text(update, 2, posted[i].title, -1, SQLITE_STATIC);
      failed = sqlite3_step(update) != SQLITE_DONE || sqlite3_changes(db) != 1;
      sqlite3_reset(update);
    }
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
    puts_(&body, "}");
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
  for (int i = 0; i < count; ++i) {
    char path[256];
    struct stat info;
    icon_path(db, rows[i].title, path, sizeof path);
    icons[i] = stat(path, &info) ? 0 : (long long)info.st_mtime;
  }
  sqlite3_close(db);
  if (count < 0) { respond_error(socket, 500, "Could not read the row."); return; }
  Buffer body = {0};
  puts_(&body, "{\"user\":");
  put_json_string(&body, user);
  puts_(&body, ",\"games\":[");
  for (int i = 0; i < count; ++i) {
    puts_(&body, i ? ",{\"titleId\":" : "{\"titleId\":");
    put_json_string(&body, rows[i].title);
    puts_(&body, ",\"name\":");
    put_json_string(&body, names[i]);
    putf(&body, ",\"index\":%lld,\"icon\":%lld}", rows[i].index, icons[i]);
  }
  puts_(&body, "]}");
  respond_json(socket, 200, &body);
  free(body.data);
}

static void get_icon(int socket, const char* title) {
  if (!is_title_id(title)) { respond_error(socket, 400, "Invalid title ID."); return; }
  char path[256];
  sqlite3* db = open_db(0);
  if (!db) { respond_error(socket, 500, "Could not open the home screen database."); return; }
  icon_path(db, title, path, sizeof path);
  sqlite3_close(db);
  int file = open(path, O_RDONLY);
  struct stat info;
  if (file < 0 || fstat(file, &info)) {
    if (file >= 0) close(file);
    respond_error(socket, 404, "This game has no icon.");
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
  } else if (get && !strncmp(target, "/icon/", 6)) {
    get_icon(socket, target + 6);
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
  }
  close(server);
  LOG("exit");
  return 0;
}

#else

// Self-check: lineupd-test [<copy of app.db> <userId>]. Reorders the copy (or a synthetic database), checks it,
// restores it.
int main(int argc, char** argv) {
  Row current[3] = {{"PPSA00001", 30}, {"PPSA00002", 20}, {"PPSA00003", 10}};
  Row posted[3] = {{"PPSA00003", 10}, {"PPSA00001", 30}, {"PPSA00002", 20}};
  long long out[3];
  assert(!plan_reorder(current, 3, posted, 3, out) && out[0] == 30 && out[1] == 20 && out[2] == 10);
  Row stale[3] = {{"PPSA00003", 11}, {"PPSA00001", 30}, {"PPSA00002", 20}};
  assert(plan_reorder(current, 3, stale, 3, out));
  Row duplicate[3] = {{"PPSA00001", 30}, {"PPSA00001", 30}, {"PPSA00002", 20}};
  assert(plan_reorder(current, 3, duplicate, 3, out));
  assert(plan_reorder(current, 3, posted, 2, out));
  Row parsed[4];
  assert(parse_order("LIN1\nPPSA00003 10\nPPSA00001 30\n", parsed, 4) == 2 && parsed[1].index == 30);
  assert(parse_order("LIN1\nPPSA0003 10\n", parsed, 4) < 0 && parse_order("XXX\n", parsed, 4) < 0);
  assert(parse_order("LIN1\nPPSA00003 10 x\n", parsed, 4) < 0);

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
  printf("logic ok, db round trip ok (%d games, backup %s)\n", count, backup);
  return 0;
}

#endif
