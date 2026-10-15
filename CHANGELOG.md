# Changelog

## 1.3.0

- Pinned games: □ pins the focused game at its place when you save. Opening other games no longer
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
