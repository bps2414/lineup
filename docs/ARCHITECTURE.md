# Architecture

[Português](pt-BR/ARQUITETURA.md)

```
app/                 the Lineup app: React/JSX, assets, manifest (titleId PPSA99077)
app/payloads/        lineupd.c, the local service, built into lineupd.elf
ps5-react/           the PS5 React framework (half144), with Payloads.start added
scripts/             test, build and package
.github/workflows/   CI and tagged releases
```

## Components

```
┌──────────── PS5 ─────────────────────────────────────────────────────┐
│  Lineup app (eboot.bin)                    lineupd.elf               │
│  React on QuickJS, PS5 React     HTTP      C, SQLite 3.46.1          │
│  draws the row, handles input ──────────▶  127.0.0.1:9077            │
│        │                                       │                     │
│        │ Payloads.start('lineupd.elf')         │ reads / writes      │
│        ▼                                       ▼                     │
│  ELF loader :9021 ── runs ──▶ lineupd   /system_data/priv/mms/app.db │
└──────────────────────────────────────────────────────────────────────┘
```

**The app** (`app/index.jsx`, `ui.jsx`, `api.js`, `i18n.js`) runs on PS5 React. Each position in the
row has an invisible focusable slot, so the framework's focus navigation drives movement. The games are
drawn separately and animate to their slots. `i18n.js` picks English or Brazilian Portuguese once, from
`DeviceInfo.get().language`.

The app needs `"networking": true` and `"filesystemAccess": "console"` in `app.json`. With those
set, PS5 React starts its own helper through the ELF loader at launch, and the app can reach the
service over HTTP and load the icons it serves.

**The service** (`app/payloads/lineupd.c`) is a single-threaded HTTP server. It handles one request at
a time, binds to loopback only, and exits on `POST /quit`. A home row holds a few dozen games, so it
does no caching.

**The framework addition** (`ps5-react`, native ABI v17):

- `tools/build_ps5.py` compiles each `app/payloads/<name>.c` with the SDK's `prospero-clang`, links the
  pinned SQLite amalgamation, and puts `<name>.elf` at the app folder root.
- `Payloads.start(name)` sends that ELF to the loader on `127.0.0.1:9021` and returns without waiting
  for it to run.

The reference is in [ps5-react/docs/NATIVE-API.md](../ps5-react/docs/NATIVE-API.md#payloads-abi-v17).

## Startup

1. The app calls `GET /version`. If it gets the same version as `api.js` `VERSION`, it goes on.
2. If another version answers, the app asks it to quit (`POST /quit`). It also sends `POST /sair` with
   `X-Fileira: 1`, which stops the service from before the rename.
3. Otherwise the app calls `Payloads.start('lineupd.elf')` and polls `/version` for up to 5 seconds.
4. It reads `/profiles`, picks the foreground user (`Users.getForeground()`), and reads that user's
   `/row`.

`VERSION` in `api.js` and `LINEUPD_VERSION` in `lineupd.c` must match. The release workflow checks
both against the tag.

## Service API

All routes are on `127.0.0.1:9077` and answer JSON unless noted. A POST, and every `/update/` route, must
carry `X-Lineup: 1`, so a web page cannot trigger one with a plain form.

| Route | Answer |
| --- | --- |
| `GET /version` | `{"version"}` |
| `GET /profiles` | `{"profiles":[{"userId","hex","name","avatar"}]}`; `avatar` is the picture's mtime, or 0 |
| `GET /row?user=<10 digits>` | `{"user","pins","games":[{"titleId","name","index","icon","pinned"}]}`, in home order; `pinned` is the slot or `null`, `pins` is `"on"`, `"missing"` (pins without their triggers) or `"off"` |
| `GET /icon/<titleId>` | `image/png`, the icon the home screen draws |
| `GET /avatar/<10 digits>` | `image/png`, `avatar.png` from `/system_data/priv/cache/profile/0x<id>/` (8 or 16 hex digits) |
| `POST /row?user=<10 digits>` | body `LIN1\n` then `<titleId> <index>[ pin]\n` per game, in the new order, with the index as read; ` pin` pins the game at that place. `{"ok","backup","integrity"}` |
| `POST /pins/off?user=<10 digits>` | `{"ok","backup"}`: unpins that profile's games and drops its triggers |
| `POST /pins/uninstall` | `{"ok","backup"}`: removes every pin, trigger and table Lineup added to `app.db` |
| `POST /quit` | `{"ok":true}`, then exits |
| `GET /update/install` | `{"kind":"folder"\|"image","path","previous"}`: the one `PPSA99077` folder or `Lineup-*.exfat` image, and the version kept in `/data/lineup/update/previous/` |
| `POST /update/apply?version=<x.y.z\|previous>` | `{"ok":true}`, then waits for the app to close, swaps the files and opens it again (see Updates) |

Errors are `{"error": "<English detail>"}`:

- 400: malformed request.
- 403: the `X-Lineup` header is missing.
- 404: no such profile or icon.
- 409: the row changed since it was read.
- 500: a database failure.

The app shows its own localized title and adds the detail after it.

## Pinned games

The system moves a game to the front each time it opens. To keep a pinned game in its place, lineupd adds
three triggers to the profile's `tbl_iconinfo_<user>` in `app.db`: `lineup_pin_u_<user>` (after an update that
raises `lastAccessIndex`, which opening a game does twice), `lineup_pin_i_<user>` (install) and
`lineup_pin_d_<user>` (uninstall). They run inside the system's own write and reorder the row before the home
screen reads it again: pinned games keep their slots (packed at the end if the row gets shorter), and the others
fill the free places by how recently they were used. Like saving, the reorder only reuses indices the row
already holds. The slots live in `lineup_pin`; `lineup_seq` and `lineup_tmp` are helper tables. The SQL avoids
CTEs, window functions, UPSERT and `RETURNING`.

Saving drops the profile's triggers, writes the order and the pins, and creates the triggers again when there
are pins, all in one transaction. When the last pin of every profile is gone, the tables go too. Rebuilding the
database can drop the triggers; `/row` then says `"missing"` and the app offers to pin the games again.

## Updates

The app reads `https://github.com/bps2414/lineup/releases/latest/download/update.json` (written by
`scripts/package.sh`: version, notes, and name, size and SHA-256 of the zip and the `.exfat`) when it
opens, at most once a day, and from Options → Updates. Settings are in `/data/lineup/settings.json`.

1. `Downloads.enqueue` fetches the zip (folder install) or the `.exfat` (image install) into
   `/data/lineup/update/<version>/`, checked against the manifest's size and SHA-256.
2. A zip is unpacked with `Archives.extract` into `stage/`, and `PPSA99077/sce_sys/param.json` (title ID,
   version), `eboot.bin` and `lineupd.elf` are checked.
3. `POST /update/apply`. lineupd finds the install and the source itself, refuses a version that is not
   newer, answers, and waits for `sceLncUtilGetAppId` to go negative (30 s at most) plus 500 ms.
   - Folder: copies it to `/data/lineup/update/previous/`, then writes each new file as `.<name>.new`,
     flushes and renames it over the old one, and removes what the new version lacks. On failure it puts
     the saved copy back.
   - Image: refuses while it is still mounted (`persistent_image_mounts`), moves the old image to
     `previous/` and the new one into its folder, then waits 25 s for ShadowMountPlus.
   - Then `sceLncUtilLaunchApp` for the foreground user. A failure is left in
     `/data/lineup/update/error`, and the app shows it.
4. The new app replaces lineupd (`ensureService`), writes `/data/lineup/update/ok-<version>` and says it
   was updated. Going back uses `previous/` the same way.

## How the home screen stores the row

All of this was established by reading and editing a copy of the database, then checking on the
console (firmware 13.60):

- **Where the row lives.** The row is in `/system_data/priv/mms/app.db` (SQLite), one table per profile:
  `tbl_iconinfo_<userId>`.
- **Profile IDs.** `userId` is the profile's folder name under `/user/home/`, read as hex and written
  as a 10-digit decimal: `12f0d5ab` becomes `0317773227`. The profile name is in
  `/user/home/<hex>/username.dat`.
- **Which rows are on the home row.** Rows with `visible = 1` and `dispLocation = 138` are on it. Other
  locations, such as `188`, are not.
- **The order.** It is `lastAccessIndex`, largest first. `lastAccessTime` plays no part.
- **Opening or closing a game.** The system gives that title a new, larger index, which is why
  opening a game moves it to the front.
- **Why saving reuses indices.** The home screen keeps an index counter in memory. A save therefore
  hands the posted order the indices the row already holds, sorted largest first, and never invents a
  larger one.
- **When the change shows.** The home screen reads the table again when an app closes, Lineup
  included. It also does so after Settings is opened and closed. That is why saving closes Lineup on
  its own.
- **Icons.** Icons come from `tbl_contentinfo.icon0Info` (a `/user/...` path), falling back to
  `/user/appmeta/<titleId>/icon0.png`.
- **The 12-game limit.** The home screen shows the first 12 games of the row. The rest are only in the
  Game Library. The limit was seen on one console and is `HOME_LIMIT` in `app/index.jsx`.

## Saving

`save_order` in `lineupd.c`:

1. `PRAGMA quick_check`. If it fails, nothing is written.
2. `VACUUM INTO /data/lineup/backups/app-<time>.db`, keeping the 5 newest backups.
3. `BEGIN IMMEDIATE` and read the row again. The posted list must be exactly the current games with the
   indices the app read. Otherwise the service answers 409 and rolls back.
4. Update each game's `lastAccessIndex`, check that exactly one row changed per game, and `COMMIT`.
5. `PRAGMA integrity_check`. The result is returned and logged.

## Tests

`scripts/test.sh` builds `lineupd.c` for the host with `-DLINEUPD_TEST` and runs the self-check:

- the reorder logic, and the parser, against malformed input;
- with no arguments, a round trip on a synthetic `app.db`: reorder, refuse a stale save, restore,
  compare;
- given `<copy of app.db> <userId>`, the same round trip on a real copy.
