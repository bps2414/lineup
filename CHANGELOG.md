# Changelog

## 1.7.0

- What you hid stays hidden across a console restart. The PS Store used to come back on its own, and if it did on
  1.6.0, opening Lineup hides it again. A hidden app that is removed and installed again also comes back hidden.
- Options no longer runs off the screen. It is shorter, its pages scroll, and the last item is always reachable.
- Options now opens on the focused app: Move and Hide are right at the top. Move shows only for apps that can
  change tabs.
- Hidden apps has its own page, with the PS Store switch and everything to bring back. It stays open after each one.
- Sorting and pinning the whole row moved to Arrange the row. Updates, the bug report and removing pins moved to More
  options.
- After a change, focus stays where it was and the message names the app.
- Bringing back an app or switching the PS Store used to drop unsaved changes to the order. It now asks you to save
  or discard them first.
- Moving between tabs is now tested across a restart, and the app stays on its new tab.

## 1.6.0

- Options, Hide or move apps. The list acts on the app focused on the row, so pick it first.
- Move to Media and Move to Games: homebrew and emulators change tabs on every profile, with everything the console
  keeps about the tab changed together, not just the tab number. Games, PS4 titles and system apps can't move, and a
  moved app loses its pin. Not tested across a console restart yet: if an app goes back to its old tab, report it.
- Hide: an app leaves its tab and library on the current profile, and it comes back from the same page. The PS Store
  has its own switch there. Remove pinned apps from every profile also brings back what was hidden.
- Every change backs up the home screen database first, in `/data/lineup/backups`. Moving also backs up `appinfo.db`.

## 1.5.0

1.4.3 was not released; its changes ship here.

- L1 and R1 switch between the Games and Media tabs. The Media tab works like the Games tab: move its apps and
  pin them (the jailbreak, Shadow Mount+, a file manager), so they stay on the home screen instead of dropping into
  the library. Each tab keeps its own pins, and one of the first 11 Media places stays free for the last app
  opened. Unsaved changes on a tab must be saved or discarded before switching.
- Opening, installing or removing an app only reorders its own tab. Pins saved by an older version switch over
  by themselves, after a backup, the first time Lineup opens.
- Options, Save a report for a bug, writes `/data/lineup/report.json`: the apps on the Games and Media tabs and
  how the home screen stores them, with each profile's number but no names or passwords. Copy it over FTP and
  attach it to your issue.

## 1.4.2

- One place on the home screen always stays unpinned. With every place pinned, the last game opened (the
  jailbreak's entry, say) had nowhere to go and ended up in the game library. Pin the whole row now leaves the
  first place free, and Lineup will not save a home screen pinned in full.
- A row pinned in full by an older version opens with an offer to free its first place.

## 1.4.1

1.4.0 was not released; its changes ship here.

- A lifted game stays on top: moving it left no longer draws the game it passes over it.
- A new version opens on its own when Lineup starts, with its notes. Not now skips that version.
- The notes fit the screen and come in Portuguese too.
- Updating copies the installed version aside before the app closes, so the swap after it is quicker.
  If you open Lineup before it is done, it waits and opens the new version by itself.
- The new version finds its service already running, so it opens without replacing it.

## 1.3.0

- Pinned games: Square pins the focused game at its place when you save. Opening other games no longer
  pushes it out; the rest of the row still follows the system's rule (the last game opened takes the
  first free place). A pin badge marks the game, a block marks it on the ruler, and the position reads
  "pinned at 3rd".
- Options: Pin the whole row, Unpin all, and Remove pinned games from every profile.
- If the system rebuilds its database and the pins come loose, Lineup offers to pin the games again.
- The change count includes pins.

## 1.2.0

- Profile pictures: in the header, under Options, and next to each name in Switch profile. A profile
  without one shows its initial.
- Updates from the app: Lineup checks for a new release once a day, shows its notes and installs it
  when you choose Update, then opens again. Options → Updates turns the daily check off, checks now,
  or goes back to the previous version. Works for folder and exFAT image installs; updating from 1.1.0
  still needs a manual install.
- Releases also ship `update.json`, the manifest the app reads.

## 1.1.0

First public release.

- The row: lift, move and drop games with the controller, for every profile on the console.
- The home screen edge: a divider in the row and a mark on the ruler after the 12th game.
- Options menu: save, discard changes, sort A to Z, switch profile.
- Saving closes the app, and the home screen comes back with the new order.
- English and Brazilian Portuguese, chosen from the system language.
- The lineupd service writes in one transaction after a backup to `/data/lineup/backups/`. If the row
  changed since it was read, the save is refused.
- Releases ship an exFAT image for ShadowMountPlus, a zip of the app folder for FTP install, and the
  full source.
