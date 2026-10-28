// SPDX-License-Identifier: GPL-3.0-or-later
// Lineup reorders the PS5 home-screen game row. The lineupd service (payloads/lineupd.c) reads and
// saves the row; this app draws it, lifts the focused game with X and carries it with the D-pad. Square pins
// the focused game where it stands when the order is saved. L1 and R1 switch between the Games and Media tabs,
// each with its own order and pins.
//
// Focus lives on invisible slots, one per position, laid out in a plain row so the focus manager moves
// between them. The games are drawn apart, keyed by title, and only their transforms move: the focused
// slot stays anchored at the left margin, and while a game is lifted, moving focus to the next slot
// carries the game there.
import {memo, useCallback, useEffect, useLayoutEffect, useRef, useState} from 'react';
import {AnimatePresence, AppRegistry, BackHandler, Controller, FocusScope, Image, Path, Svg, Text, Users, View,
  motion, useFocusActions} from '@ps5-react/core';
import {useInterfaceSounds} from './sounds.js';
import * as api from './api.js';
import {LANGUAGE, T} from './i18n.js';
import {BOTTOM, DIGITS, EM, TOP} from './digits.js';
import {Avatar, Badge, Button, C, Dialog, Hints, LiftLight, Mark, Message, PinBadge, Sheet, SheetItem, SheetRule,
  Toast, Wash, px} from './ui.jsx';

const LEFT = 64;           // the focused game's left edge
const BIG = 168;           // focused icon
const SMALL = 128;         // the others
const STRIDE = SMALL + 20; // slot pitch
const PAD = 8;             // room around the icon for the focus ring
const BOX = BIG + 2 * PAD;
const LIFT = 22;
const LIFT_SCALE = 1.06;
const BEFORE = 2;          // games drawn left of focus (off screen, ready to slide in)
const AFTER = 8;

// The home screen shows only the first apps of each tab; the rest are in the library alone.
// ponytail: 12 games seen on one console (firmware 13.60) and 11 media apps reported; read them from the system if
// they turn out to vary. HOME_LIMIT and MEDIA_HOME_LIMIT in payloads/lineupd.c match.
const HOME_LIMIT = 12;
const MEDIA_HOME_LIMIT = 11;
const homeLimit = tab => (tab === 'media' ? MEDIA_HOME_LIMIT : HOME_LIMIT);

const SPRING = {type: 'spring', stiffness: 420, damping: 40};
const TILE = {...SPRING, opacity: {type: 'tween', duration: 0.14}};

const sortKey = name => name.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
// Games moved or pinned/unpinned since the row was read. `pinned` maps the games read pinned to their slots.
const changed = s => s.order.reduce((count, id, i) => count + (s.home[id] !== i || !s.pins[id] !== !(id in s.pinned)), 0);
const allPinned = pinned => Object.fromEntries(Object.keys(pinned).map(id => [id, true]));
// Every home place of `tab` pinned: the last app opened (the jailbreak's entry, say) would have no place there and end
// up in the library, so one stays free. lineupd refuses such a save too.
const homeFull = (order, pins, tab) => order.slice(0, homeLimit(tab)).every(id => pins[id]);
const without = (pins, id) => {
  const rest = {...pins};
  delete rest[id];
  return rest;
};

// The order the pins ask for, as lineupd's triggers work it out: pinned games at their slots (packed at the end of
// a row that got shorter), the others in their order in the free places.
function pinnedOrder(order, pinned) {
  const fixed = order.filter(id => id in pinned).sort((a, b) => pinned[a] - pinned[b]);
  const at = {};
  fixed.forEach((id, k) => {
    at[Math.min(pinned[id], order.length - fixed.length + k)] = id;
  });
  const rest = order.filter(id => !(id in pinned));
  return order.map((_, i) => at[i] ?? rest.shift());
}

const Slots = memo(function Slots({count, onFocusIndex, onPressIndex}) {
  const slots = [];
  for (let i = 0; i < count; ++i) {
    slots.push(
      <View key={i} focusKey={`slot:${i}`} autoFocus={i === 0} onFocus={() => onFocusIndex(i)}
        onPress={() => onPressIndex(i)} className="absolute top-0 w-[128px] h-[168px]"
        style={{left: px(LEFT + i * STRIDE)}} />,
    );
  }
  return <View className="absolute left-0 top-[140px] w-screen h-[168px]">{slots}</View>;
});

// Each game keeps two icons mounted, decoded at the two sizes it is drawn at: 128 px, dimmed, for the
// row, and 168 px with its ring for focus. They swap with a short crossfade while scaling into each
// other, so at rest neither is scaled or faded. Tiles sit at fixed places in a strip the row slides
// as a whole, so a move re-renders only the games whose `place` changed.
const Tile = memo(function Tile({game, index, place, lifted, moved, pinned}) {
  const focused = place === 'focus';
  const up = focused && lifted;
  const x = LEFT + index * STRIDE + (place === 'after' ? BIG - SMALL : 0);
  const grow = BIG * (LIFT_SCALE - 1) / 2;
  const scale = up ? LIFT_SCALE : focused ? 1 : SMALL / BIG;
  const shiftX = up ? -grow : 0;
  const shiftY = up ? -LIFT - grow : 0;
  const uri = api.iconUrl(game);
  return (
    // The focused game paints above its neighbours, so one sliding past a lifted game goes under it.
    <motion.View className="absolute left-0 top-0" initial={false} animate={{x}} transition={SPRING}
      style={{zIndex: focused ? 1 : 0}}>
      <motion.View className="absolute left-0 top-0 w-[128px] h-[128px] origin-top-left" initial={false}
        animate={{scale: focused ? BIG / SMALL : 1, opacity: focused ? 0 : 1}} transition={TILE}>
        <View className="absolute inset-0 rounded-[10px] bg-surface p-3 justify-end">
          <Text className="font-inter text-hint text-subtle line-clamp-3">{game.name}</Text>
        </View>
        <Image source={{uri}} className="absolute inset-0 rounded-[10px]" />
        <View className={place === 'before' ? 'absolute inset-0 rounded-[10px] bg-black/75'
          : 'absolute inset-0 rounded-[10px] bg-black/40'} />
        {pinned ? <PinBadge /> : null}
      </motion.View>
      <motion.View className="absolute -left-2 -top-2 w-[184px] h-[184px] origin-top-left" initial={false}
        animate={{x: shiftX + PAD - PAD * scale, y: shiftY + PAD - PAD * scale, scale, opacity: focused ? 1 : 0}}
        transition={TILE}>
        <View className="absolute left-2 top-2 w-[168px] h-[168px] rounded-xl bg-surface p-4 justify-end">
          <Text className="font-inter text-small text-subtle line-clamp-3">{game.name}</Text>
        </View>
        <Image source={{uri}} className="absolute left-2 top-2 w-[168px] h-[168px] rounded-xl" />
        {pinned ? <PinBadge big /> : null}
        {focused && !lifted ? <View className="absolute inset-0 rounded-[20px] border-[3px] border-fg" /> : null}
      </motion.View>
      {up ? (
        <motion.View className="absolute" initial={{opacity: 0}} animate={{opacity: 1}} transition={TILE.opacity}
          style={{left: px(shiftX - PAD * LIFT_SCALE), top: px(shiftY - PAD * LIFT_SCALE),
            width: px(BOX * LIFT_SCALE), height: px(BOX * LIFT_SCALE)}}>
          <LiftLight radius={20 * LIFT_SCALE} />
        </motion.View>
      ) : null}
      {moved ? (
        <motion.View className="absolute left-0 top-0 size-[6px] rounded-full bg-fg" initial={false}
          animate={{x: (focused ? BIG : SMALL) / 2 - 3 + shiftX, y: (focused ? BIG + 14 : SMALL + 12)}}
          transition={SPRING} />
      ) : null}
    </motion.View>
  );
});

function Row({s}) {
  const limit = homeLimit(s.tab);
  const from = Math.max(0, s.focus - BEFORE);
  const ids = s.order.slice(from, s.focus + AFTER + 1);
  return (
    <motion.View className="absolute left-0 top-[140px] w-screen h-[240px]" initial={false}
      animate={{x: -s.focus * STRIDE}} transition={SPRING}>
      {ids.map((id, k) => {
        const index = from + k;
        const place = index < s.focus ? 'before' : index > s.focus ? 'after' : 'focus';
        return (
          <Tile key={id} game={s.games[id]} index={index} place={place} lifted={place === 'focus' && s.lifted}
            moved={s.home[id] !== index} pinned={!!s.pins[id]} />
        );
      })}
      {s.order.length > limit ? (
        // The home screen's edge, centred in the gap before the first game it leaves out.
        <motion.View className="absolute left-0 top-0 w-[2px] h-[128px] bg-muted" initial={false}
          animate={{x: LEFT + limit * STRIDE + (limit > s.focus ? BIG - SMALL : 0) - (STRIDE - SMALL) / 2 - 1}}
          transition={SPRING} />
      ) : null}
    </motion.View>
  );
}

const RAIL = 1152;

// The whole row at a glance: a hairline ruler with a tick per game (when they fit), the moved games'
// ticks in white, the pinned ones as small white blocks, and a marker on the focused one.
const Ticks = memo(function Ticks({order, home, pins, limit}) {
  const at = i => Math.round(i * RAIL / Math.max(order.length - 1, 1));
  if (order.length > 96) return null;
  const ticks = order.map((id, i) => (pins[id]
    ? <View key={id} className="absolute top-[4px] w-[6px] h-[8px] rounded-[1px] bg-fg" style={{left: px(at(i) - 3)}} />
    : <View key={id} className={home[id] === i ? 'absolute top-[5px] w-[2px] h-[6px] bg-dim'
      : 'absolute top-[3px] w-[2px] h-[10px] bg-fg'} style={{left: px(at(i) - 1)}} />
  ));
  // Where the home screen stops: a tall mark halfway between the last game it shows and the next.
  if (order.length > limit) {
    ticks.push(<View key="home-limit" className="absolute -top-[6px] w-[2px] h-[28px] bg-muted"
      style={{left: px(Math.round((at(limit - 1) + at(limit)) / 2) - 1)}} />);
  }
  return ticks;
});

function Rail({s}) {
  const x = Math.round(s.focus * RAIL / Math.max(s.order.length - 1, 1));
  return (
    <View className="absolute left-16 top-[440px] w-[1152px] h-[16px]">
      <View className="absolute left-0 right-0 top-[7px] h-[2px] bg-line" />
      <Ticks order={s.order} home={s.home} pins={s.pins} limit={homeLimit(s.tab)} />
      <motion.View className="absolute left-0 top-0 w-[6px] h-[16px] rounded-full bg-fg" initial={false}
        animate={{x: x - 3}} transition={SPRING} />
    </View>
  );
}

/** A number set in Inter's digit outlines at `size` logical px, its box from the digits' top to baseline. */
function Numeral({value, size = 220}) {
  const unit = px(size) / EM;
  const height = Math.round((BOTTOM - TOP) * unit);
  return (
    <View className="flex-row">
      {[...String(value)].map((digit, i) => {
        const [advance, d] = DIGITS[digit];
        return (
          <Svg key={i} width={Math.round(advance * unit)} height={height} viewBox={`0 ${TOP} ${advance} ${BOTTOM - TOP}`}
            style={i ? {marginLeft: -px(size * 0.035)} : null}>
            <Path d={d} fill={C.fg} />
          </Svg>
        );
      })}
    </View>
  );
}

/** The focused game's name and its position, the screen's one big number. */
function Position({s}) {
  const id = s.order[s.focus];
  const game = s.games[id];
  const was = s.home[id];
  const notes = [];
  if (s.pins[id]) notes.push(T.pins.pinnedAt(s.focus + 1));
  if (was !== s.focus) notes.push(T.before(was + 1));
  if (s.focus >= homeLimit(s.tab)) notes.push(T.offHome);
  const note = notes.join(', ');
  return (
    <>
      <Text className="absolute left-16 top-[354px] w-[1000px] font-inter text-title text-fg truncate">
        {game.name}
      </Text>
      <View className="absolute left-16 bottom-[52px] flex-row items-end gap-6">
        <motion.View key={s.focus + 1} initial={{opacity: 0, x: 26 * s.dir}} animate={{opacity: 1, x: 0}}
          transition={{type: 'tween', duration: 0.2, ease: 'easeOut'}}>
          <Numeral value={s.focus + 1} />
        </motion.View>
        <View className="gap-1">
          <Text className="font-inter text-small text-subtle">{note || ' '}</Text>
          <Text className="font-inter text-of text-muted">{T.of(s.order.length)}</Text>
        </View>
      </View>
    </>
  );
}

const Shoulder = ({label}) => (
  <View className="rounded-md border-[1.5px] border-dim px-1.5">
    <Text className="font-inter text-hint text-fg">{label}</Text>
  </View>
);

/** The two tabs of the home screen, as L1 and R1 switch them; the one shown in white. */
function Tabs({tab}) {
  return (
    <View className="absolute right-16 top-10 flex-row items-center gap-4">
      <Shoulder label="L1" />
      <Text className={`font-inter text-head ${tab === 'games' ? 'text-fg' : 'text-dim'}`}>{T.tabs.games}</Text>
      <Text className={`font-inter text-head ${tab === 'media' ? 'text-fg' : 'text-dim'}`}>{T.tabs.media}</Text>
      <Shoulder label="R1" />
    </View>
  );
}

function Header({profile, tab}) {
  return (
    <>
      <View className="absolute left-16 top-10 flex-row items-center gap-4">
        <Mark />
        <Text className="font-inter text-head text-fg">Lineup</Text>
        {profile ? <Avatar profile={profile} size={36} /> : null}
        {profile ? <Text className="font-inter text-head text-muted">{profile.name}</Text> : null}
      </View>
      {tab ? <Tabs tab={tab} /> : null}
    </>
  );
}

const H = T.hints;
const ROW_HINTS = [['cross', H.lift], ['triangle', H.save], ['options', H.options], ['circle', H.quit]];
const LIFT_HINTS = [['dpad', H.move], ['cross', H.drop], ['circle', H.cancel]];
const PICK_HINTS = [['cross', H.choose], ['circle', H.back]];

const START = {phase: 'loading', step: T.service.looking, error: null, profiles: [], user: null, tab: 'games',
  games: {}, home: {}, base: [], order: [], pins: {}, pinned: {}, focus: 0, dir: 1, lifted: false, liftFrom: 0, layer: null,
  target: null, toast: null, pending: null, progress: null, settings: null, update: null, install: null};

function App() {
  const [s, setS] = useState(START);
  const ref = useRef(s);
  ref.current = s;
  const patch = useCallback(next => setS(previous => ({...previous, ...(typeof next === 'function' ? next(previous) : next)})), []);
  const {focus} = useFocusActions();
  useInterfaceSounds();

  const dirty = s.phase === 'row' && changed(s) > 0;
  useLayoutEffect(() => {
    globalThis.__lineupState = {phase: s.phase, focus: s.focus, lifted: s.lifted, dirty, count: s.order.length,
      layer: s.layer};
  }, [s.phase, s.focus, s.lifted, dirty, s.order.length, s.layer]);

  useEffect(() => {
    if (!s.pending) return undefined;
    const timer = setTimeout(() => focus(`slot:${s.pending.index}`), 0);
    return () => clearTimeout(timer);
  }, [s.pending]);
  useEffect(() => {
    if (!s.toast) return undefined;
    const timer = setTimeout(() => patch({toast: null}), 3200);
    return () => clearTimeout(timer);
  }, [s.toast]);
  useEffect(() => () => Controller.resetLightBar(), []);
  // The home screen rereads the row when an app closes, so a saved order closes the app by itself.
  useEffect(() => {
    if (s.phase !== 'saved') return undefined;
    const timer = setTimeout(exit, 1800);
    return () => clearTimeout(timer);
  }, [s.phase]);

  const toast = text => patch({toast: {text, at: Date.now()}});
  // Closing a layer hands focus back to the focused game; the focus manager alone picks the slot nearest
  // the sheet's last item.
  const close = () => patch(p => ({layer: null, pending: {index: p.focus}}));
  const light = on => (on ? Controller.setLightBar('#ffffff') : Controller.resetLightBar());
  const exit = () => {
    Controller.resetLightBar();
    BackHandler.exitApp();
  };

  const loadRow = async (user, profiles, tab = ref.current.tab) => {
    const {games: list, pins} = await api.row(user, tab);
    if (!list.length) throw new api.Problem(...(tab === 'media' ? T.tabs.emptyMedia : T.emptyRow));
    const games = {};
    const home = {};
    const pinned = {};
    list.forEach((game, i) => {
      games[game.titleId] = game;
      home[game.titleId] = i;
      if (game.pinned !== null && game.pinned !== undefined) pinned[game.titleId] = game.pinned;
    });
    const base = list.map(game => game.titleId);
    // A row pinned in full by 1.4.1 or older.
    const layer = pins === 'missing' ? 'pinsMissing' : pins === 'on' && homeFull(base, allPinned(pinned), tab) ? 'pinsFull' : null;
    light(false);
    patch({phase: 'row', profiles, user, tab, games, home, base, order: base, pins: allPinned(pinned), pinned, focus: 0,
      dir: 1, lifted: false, layer, target: null, pending: layer ? null : {index: 0}});
  };

  const boot = async () => {
    patch({...START, phase: 'loading'});
    try {
      if (api.applying()) patch({step: T.update.finishing});
      if (await api.finishUpdate()) {
        // This copy of the app is the version just replaced: open the new one.
        Controller.resetLightBar();
        if (BackHandler.exitApp({relaunch: true})) return;
        throw new api.Problem(...T.update.reopen);
      }
      await api.ensureService(step => patch({step}));
      const profiles = await api.profiles();
      if (!profiles.length) {
        throw new api.Problem(...T.noProfiles);
      }
      // Decoded ahead at the profile list's size, so the list opens with its pictures.
      for (const profile of profiles) {
        if (profile.avatar) Image.prefetch(api.avatarUrl(profile), {width: px(40), height: px(40)}).catch(() => {});
      }
      const foreground = Users.getForeground();
      const id = foreground ? String(foreground.id >>> 0).padStart(10, '0') : '';
      await loadRow((profiles.find(profile => profile.userId === id) ?? profiles[0]).userId, profiles, 'games');
    } catch (error) {
      patch({phase: 'error', error: error instanceof api.Problem ? error
        : {title: T.openFailed, detail: String(error?.message ?? error)}});
      return;
    }
    afterOpen();
  };
  useEffect(() => {
    boot();
  }, []);

  const keepSettings = settings => {
    api.saveSettings(settings);
    patch({settings});
  };

  // In the background once the row is up: how the last update went, then a check, whose news opens at once
  // unless the user put that version off or is already busy with something.
  const afterOpen = async () => {
    const outcome = api.updateOutcome();
    if (outcome) toast(outcome);
    const settings = api.settings();
    patch({settings});
    api.install().then(install => patch({install}), error => patch({install: {error: error.message}}));
    if (!settings.checkOnOpen) return;
    try {
      const update = await api.checkUpdate();
      if (!update) return;
      const now = ref.current;
      if (now.settings.skipped === update.version || now.phase !== 'row' || now.layer || now.lifted) {
        patch({update});
        toast(T.update.available(update.version));
      } else patch({update, layer: 'update'});
    } catch {
      // Offline, say: the next opening tries again.
    }
  };

  const later = () => {
    keepSettings({...ref.current.settings, skipped: ref.current.update.version});
    close();
  };

  const checkNow = async () => {
    patch({layer: 'busy'});
    try {
      const update = await api.checkUpdate();
      if (update) patch({update, layer: 'update'});
      else {
        close();
        toast(T.update.upToDate);
      }
    } catch (error) {
      close();
      toast(error.message);
    }
  };

  // Downloads and prepares the update, hands the swap to lineupd and closes; lineupd opens the app again.
  const runUpdate = async version => {
    const {update, install} = ref.current;
    light(false);
    patch({lifted: false, layer: 'busy', progress: T.update.preparing});
    try {
      if (!install || install.error) throw new Error(install?.error ?? T.update.noAnswer);
      if (version !== 'previous') await api.download(update, install.kind, progress => patch({progress}));
      patch({progress: T.update.backingUp});
      await api.apply(version);
      patch({progress: T.update.applying});
      exit();
    } catch (error) {
      patch({progress: null});
      close();
      toast(error.message);
    }
  };

  const switchTo = async user => {
    patch({layer: 'busy'});
    try {
      await loadRow(user, ref.current.profiles);
    } catch (error) {
      close();
      toast(error.detail ?? error.message);
    }
  };

  // Saves, then shows the saved screen, or `next()` instead when switching profiles. `force` saves an unchanged
  // row too, to put the pins' triggers back.
  const save = async (next, now = ref.current, force = false) => {
    if (!changed(now) && !force) {
      close();
      toast(T.alreadySaved);
      return;
    }
    if (homeFull(now.order, now.pins, now.tab)) {
      close();
      toast(T.pins.keepFree);
      return;
    }
    light(false);
    patch({lifted: false, layer: 'busy'});
    const result = await api.save(now.user, now.tab, now.order, now.games, now.pins);
    if (result.ok) {
      if (next) {
        toast(T.savedToast);
        next();
      } else patch({phase: 'saved', layer: null});
      return;
    }
    toast(result.message);
    if (!result.stale) {
      close();
      return;
    }
    try {
      await loadRow(now.user, now.profiles);
    } catch (error) {
      patch({phase: 'error', error});
    }
  };

  const onFocusIndex = useCallback(index => patch(p => {
    if (!p.lifted || index === p.focus) return {focus: index, dir: index < p.focus ? -1 : 1};
    const order = p.order.slice();
    const [id] = order.splice(p.focus, 1);
    order.splice(index, 0, id);
    return {order, focus: index, dir: index < p.focus ? -1 : 1};
  }), []);
  const onPressIndex = useCallback(index => {
    const lifting = !ref.current.lifted;
    if (lifting) Controller.vibrate(0.35, 45);
    light(lifting);
    patch({lifted: lifting, liftFrom: index});
  }, []);

  const cancelLift = p => {
    const order = p.order.slice();
    const [id] = order.splice(p.focus, 1);
    order.splice(p.liftFrom, 0, id);
    light(false);
    return {order, lifted: false, focus: p.liftFrom, dir: p.liftFrom < p.focus ? -1 : 1,
      pending: {index: p.liftFrom}};
  };
  const onBack = () => {
    const p = ref.current;
    if (p.lifted) patch(cancelLift);
    else if (changed(p)) patch({layer: 'exit'});
    else exit();
    return true;
  };
  // Unsaved changes stay on their tab: switching asks to save or discard them first.
  const switchTab = async tab => {
    const p = ref.current;
    if (tab === p.tab || p.lifted) return;
    if (changed(p)) {
      toast(T.tabs.saveFirst);
      return;
    }
    patch({layer: 'busy'});
    try {
      await loadRow(p.user, p.profiles, tab);
    } catch (error) {
      close();
      toast(error.detail ?? error.message);
    }
  };
  const onAction = action => {
    if (action === 'options') {
      light(false);
      patch({lifted: false, layer: 'menu'});
      return true;
    }
    if (action === 'l1' || action === 'r1') {
      switchTab(action === 'l1' ? 'games' : 'media');
      return true;
    }
    if (action === 'triangle') {
      save();
      return true;
    }
    if (action === 'square') {
      const p = ref.current;
      const id = p.order[p.focus];
      const pins = p.pins[id] ? without(p.pins, id) : {...p.pins, [id]: true};
      if (!p.pins[id] && homeFull(p.order, pins, p.tab)) toast(T.pins.keepFree);
      else patch({pins});
      return true;
    }
    return false;
  };

  const discard = () => {
    patch(p => ({order: p.base, pins: allPinned(p.pinned), layer: null, pending: {index: 0}}));
    toast(T.discarded);
  };
  // The whole row but its first place, which stays free.
  const pinAll = on => patch(p => ({pins: on ? Object.fromEntries(p.order.slice(1).map(id => [id, true])) : {},
    layer: null, pending: {index: p.focus}}));
  // Puts the games back at their slots and saves, which creates the triggers again. A row pinned in full gets its
  // first place freed.
  const reapply = () => {
    const now = ref.current;
    const order = pinnedOrder(now.order, now.pinned);
    const pins = homeFull(order, now.pins, now.tab) ? without(now.pins, order[0]) : now.pins;
    patch({order, pins});
    save(null, {...now, order, pins}, true);
  };
  const unpinEverywhere = async () => {
    patch({layer: 'busy'});
    const error = await api.unpinAll();
    if (!error) patch({pins: {}, pinned: {}});
    close();
    toast(error || T.pins.removed);
  };
  const saveReport = async () => {
    patch({layer: 'busy'});
    const {path, error} = await api.report();
    if (path) patch({layer: 'report', report: path});
    else {
      close();
      toast(error);
    }
  };
  const sortAZ = () => patch(p => {
    const id = p.order[p.focus];
    const order = p.order.slice().sort((a, b) => {
      const x = sortKey(p.games[a].name);
      const y = sortKey(p.games[b].name);
      return x < y ? -1 : x > y ? 1 : 0;
    });
    return {order, layer: null, pending: {index: order.indexOf(id)}};
  });
  const pickProfile = user => {
    const p = ref.current;
    if (user === p.user) close();
    else if (changed(p)) patch({layer: 'switch', target: user});
    else switchTo(user);
  };

  const profile = s.profiles.find(item => item.userId === s.user);

  if (s.phase === 'loading') {
    return (
      <View className="w-screen h-screen bg-canvas">
        <Wash />
        <View className="absolute left-16 top-[300px] gap-5">
          <Mark breathing />
          <Text className="font-inter text-big text-fg">Lineup</Text>
          <Text className="font-inter text-body text-muted">{s.step}</Text>
        </View>
      </View>
    );
  }
  if (s.phase === 'error' || s.phase === 'saved') {
    const saved = s.phase === 'saved';
    const back = () => {
      exit();
      return true;
    };
    return (
      <View className="w-screen h-screen bg-canvas">
        <Wash />
        <Header profile={profile} />
        <FocusScope autoFocus onBack={back} onAction={() => true}>
          {saved ? (
            <Message icon={<Badge kind="ok" />} title={T.saved[0]}
              body={T.saved[1]}>
              <Button label={T.closeNow} autoFocus onPress={exit} />
            </Message>
          ) : (
            <Message icon={<Badge kind="error" />} title={s.error.title} body={s.error.detail}>
              <Button label={T.retry} autoFocus onPress={boot} />
              <Button label={T.closeApp} onPress={exit} />
            </Message>
          )}
        </FocusScope>
        <Hints items={[['cross', H.choose], ['circle', H.close]]} />
      </View>
    );
  }

  const count = changed(s);
  const pinHint = ['square', s.pins[s.order[s.focus]] ? H.unpin : H.pin];
  const hints = s.layer ? PICK_HINTS : s.lifted ? LIFT_HINTS : [ROW_HINTS[0], pinHint, ...ROW_HINTS.slice(1)];
  const settings = s.settings ?? {checkOnOpen: true, skipped: ''};
  const file = s.update?.[s.install?.kind === 'image' ? 'exfat' : 'zip'];
  const notes = [s.update?.notes?.[LANGUAGE] ?? s.update?.notes?.en, file?.size ? T.update.size(file.size) : '']
    .filter(Boolean).join('\n\n');
  return (
    <View className="w-screen h-screen bg-canvas">
      <Wash />
      <Header profile={profile} tab={s.tab} />
      <FocusScope row autoFocus onBack={onBack} onAction={onAction}>
        <Slots count={s.order.length} onFocusIndex={onFocusIndex} onPressIndex={onPressIndex} />
      </FocusScope>
      <Row s={s} />
      <Rail s={s} />
      <Position s={s} />
      {s.layer === 'menu' || s.layer === 'profiles' || s.layer === 'updates' ? (
        // One sheet for every page, so it stays put and still knows which game to give focus back to.
        <Sheet page={s.layer} onClose={close}
          onBack={() => (s.layer === 'menu' ? close() : patch({layer: 'menu'}))}
          title={s.layer === 'menu' ? T.menu.title : s.layer === 'profiles' ? T.profilesTitle : T.updates.title}
          subtitle={s.layer === 'menu' ? profile?.name : s.layer === 'profiles' ? T.profilesSubtitle : `Lineup ${api.VERSION}`}
          leading={s.layer === 'menu' && profile ? <Avatar profile={profile} size={24} /> : null}>
          {s.layer === 'updates' ? [
            <SheetItem key="open" label={T.updates.checkOnOpen} checked={settings.checkOnOpen}
              onPress={() => keepSettings({...settings, checkOnOpen: !settings.checkOnOpen})} />,
            <SheetItem key="now" label={T.updates.checkNow} onPress={checkNow} />,
            s.install?.previous ? (
              <SheetItem key="back" label={T.updates.rollback} detail={s.install.previous}
                onPress={() => patch({layer: 'rollback'})} />
            ) : null,
          ] : s.layer === 'menu' ? [
            <SheetItem key="save" label={T.menu.save} onPress={() => save()}
              detail={T.menu.changes(count)} />,
            <SheetItem key="discard" label={T.menu.discard} onPress={discard} />,
            <SheetItem key="sort" label={T.menu.sort} onPress={sortAZ} />,
            <SheetItem key="pin-all" label={T.pins.pinAll} onPress={() => pinAll(true)} />,
            <SheetItem key="unpin-all" label={T.pins.unpinAll} onPress={() => pinAll(false)} />,
            <SheetRule key="rule" />,
            <SheetItem key="profiles" label={T.menu.profiles} onPress={() => patch({layer: 'profiles'})}
              detail={s.profiles.length > 1 ? T.menu.profileCount(s.profiles.length) : ''} />,
            s.update ? (
              <SheetItem key="update" label={T.menu.updateTo(s.update.version)} onPress={() => patch({layer: 'update'})} />
            ) : null,
            <SheetItem key="updates" label={T.menu.updates} onPress={() => patch({layer: 'updates'})} />,
            <SheetItem key="remove-pins" label={T.pins.removeAll} onPress={() => patch({layer: 'removePins'})} />,
            <SheetItem key="report" label={T.report.save} onPress={saveReport} />,
          ] : s.profiles.map(item => (
            <SheetItem key={item.userId} label={item.name} checked={item.userId === s.user}
              leading={<Avatar profile={item} size={40} />}
              autoFocus={item.userId === s.user} onPress={() => pickProfile(item.userId)} />
          ))}
        </Sheet>
      ) : null}
      {s.layer === 'exit' ? (
        <Dialog title={T.exitDialog.title} onBack={close} body={T.exitDialog.body(count)}>
          <Button label={T.exitDialog.save} onPress={() => save()} />
          <Button label={T.exitDialog.discard} onPress={exit} />
          <Button label={T.exitDialog.back} onPress={close} />
        </Dialog>
      ) : null}
      {s.layer === 'switch' ? (
        <Dialog title={T.switchDialog.title} body={T.switchDialog.body}
          onBack={() => patch({layer: 'profiles'})}>
          <Button label={T.switchDialog.save} onPress={() => save(() => switchTo(s.target))} />
          <Button label={T.switchDialog.discard} onPress={() => switchTo(s.target)} />
          <Button label={T.switchDialog.back} onPress={() => patch({layer: 'profiles'})} />
        </Dialog>
      ) : null}
      {s.layer === 'update' && s.update ? (
        <Dialog title={T.update.title(s.update.version)} onBack={later} body={notes} wide>
          <Button label={T.update.install} onPress={() => runUpdate(s.update.version)} />
          <Button label={T.update.later} onPress={later} />
        </Dialog>
      ) : null}
      {s.layer === 'rollback' ? (
        <Dialog title={T.update.rollbackTitle(s.install?.previous ?? '')} body={T.update.rollbackBody}
          onBack={() => patch({layer: 'updates'})}>
          <Button label={T.update.rollbackConfirm} onPress={() => runUpdate('previous')} />
          <Button label={T.update.cancel} onPress={() => patch({layer: 'updates'})} />
        </Dialog>
      ) : null}
      {s.layer === 'removePins' ? (
        <Dialog title={T.pins.removeTitle} body={T.pins.removeBody} onBack={() => patch({layer: 'menu'})}>
          <Button label={T.pins.removeConfirm} onPress={unpinEverywhere} />
          <Button label={T.pins.cancel} onPress={() => patch({layer: 'menu'})} />
        </Dialog>
      ) : null}
      {s.layer === 'report' ? (
        <Dialog title={T.report.title} body={T.report.body(s.report)} onBack={close}>
          <Button label={T.report.ok} onPress={close} />
        </Dialog>
      ) : null}
      {s.layer === 'pinsMissing' ? (
        <Dialog title={T.pins.missingTitle} body={T.pins.missingBody} onBack={close}>
          <Button label={T.pins.reapply} onPress={reapply} />
          <Button label={T.pins.later} onPress={close} />
        </Dialog>
      ) : null}
      {s.layer === 'pinsFull' ? (
        <Dialog title={T.pins.fullTitle} body={T.pins.fullBody} onBack={close}>
          <Button label={T.pins.freeFirst} onPress={reapply} />
          <Button label={T.pins.later} onPress={close} />
        </Dialog>
      ) : null}
      {s.layer === 'busy' ? (
        <FocusScope trap autoFocus onBack={() => true} onAction={() => true}>
          <View className="absolute inset-0 bg-black/70 items-center justify-center">
            <View focusable className="flex-row items-center gap-4 rounded-2xl bg-surface border-2 border-line px-8 py-6">
              <Mark breathing />
              <Text className="font-inter text-body text-fg">{s.progress ?? T.busy}</Text>
            </View>
          </View>
        </FocusScope>
      ) : null}
      <Hints items={hints} />
      <AnimatePresence>{s.toast ? <Toast key={s.toast.at} text={s.toast.text} /> : null}</AnimatePresence>
    </View>
  );
}

AppRegistry.registerComponent('lineup', () => App);
