// SPDX-License-Identifier: GPL-3.0-or-later
// Fileira reorders the PS5 home-screen game row. The fileirad service (payloads/fileirad.c) reads and
// saves the row; this app draws it, lifts the focused game with X and carries it with the D-pad.
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
import {BOTTOM, DIGITS, EM, TOP} from './digits.js';
import {Badge, Button, C, Dialog, Hints, LiftLight, Mark, Message, Sheet, SheetItem, SheetRule, Toast, Wash,
  px} from './ui.jsx';

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

// The home screen shows only the first games of the row; the rest are in the game library alone.
// ponytail: 12 seen on one console (firmware 13.60); read it from the system if it turns out to vary.
const HOME_LIMIT = 12;

const SPRING = {type: 'spring', stiffness: 420, damping: 40};
const TILE = {...SPRING, opacity: {type: 'tween', duration: 0.14}};

const ordinal = n => `${n}º`;
const sortKey = name => name.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
const changed = s => s.order.reduce((count, id, i) => count + (s.home[id] !== i), 0);

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
const Tile = memo(function Tile({game, index, place, lifted, moved}) {
  const focused = place === 'focus';
  const up = focused && lifted;
  const x = LEFT + index * STRIDE + (place === 'after' ? BIG - SMALL : 0);
  const grow = BIG * (LIFT_SCALE - 1) / 2;
  const scale = up ? LIFT_SCALE : focused ? 1 : SMALL / BIG;
  const shiftX = up ? -grow : 0;
  const shiftY = up ? -LIFT - grow : 0;
  const uri = api.iconUrl(game);
  return (
    <motion.View className="absolute left-0 top-0" initial={false} animate={{x}} transition={SPRING}>
      <motion.View className="absolute left-0 top-0 w-[128px] h-[128px] origin-top-left" initial={false}
        animate={{scale: focused ? BIG / SMALL : 1, opacity: focused ? 0 : 1}} transition={TILE}>
        <View className="absolute inset-0 rounded-[10px] bg-surface p-3 justify-end">
          <Text className="font-inter text-hint text-subtle line-clamp-3">{game.nome}</Text>
        </View>
        <Image source={{uri}} className="absolute inset-0 rounded-[10px]" />
        <View className={place === 'before' ? 'absolute inset-0 rounded-[10px] bg-black/75'
          : 'absolute inset-0 rounded-[10px] bg-black/40'} />
      </motion.View>
      <motion.View className="absolute -left-2 -top-2 w-[184px] h-[184px] origin-top-left" initial={false}
        animate={{x: shiftX + PAD - PAD * scale, y: shiftY + PAD - PAD * scale, scale, opacity: focused ? 1 : 0}}
        transition={TILE}>
        <View className="absolute left-2 top-2 w-[168px] h-[168px] rounded-xl bg-surface p-4 justify-end">
          <Text className="font-inter text-small text-subtle line-clamp-3">{game.nome}</Text>
        </View>
        <Image source={{uri}} className="absolute left-2 top-2 w-[168px] h-[168px] rounded-xl" />
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
            moved={s.home[id] !== index} />
        );
      })}
      {s.order.length > HOME_LIMIT ? (
        // The home screen's edge, centred in the gap before the first game it leaves out.
        <motion.View className="absolute left-0 top-0 w-[2px] h-[128px] bg-muted" initial={false}
          animate={{x: LEFT + HOME_LIMIT * STRIDE + (HOME_LIMIT > s.focus ? BIG - SMALL : 0) - (STRIDE - SMALL) / 2 - 1}}
          transition={SPRING} />
      ) : null}
    </motion.View>
  );
}

const RAIL = 1152;

// The whole row at a glance: a hairline ruler with a tick per game (when they fit), the moved games'
// ticks in white and a marker on the focused one.
const Ticks = memo(function Ticks({order, home}) {
  const at = i => Math.round(i * RAIL / Math.max(order.length - 1, 1));
  if (order.length > 96) return null;
  const ticks = order.map((id, i) => (
    <View key={id} className={home[id] === i ? 'absolute top-[5px] w-[2px] h-[6px] bg-dim'
      : 'absolute top-[3px] w-[2px] h-[10px] bg-fg'} style={{left: px(at(i) - 1)}} />
  ));
  // Where the home screen stops: a tall mark halfway between the last game it shows and the next.
  if (order.length > HOME_LIMIT) {
    ticks.push(<View key="home-limit" className="absolute -top-[6px] w-[2px] h-[28px] bg-muted"
      style={{left: px(Math.round((at(HOME_LIMIT - 1) + at(HOME_LIMIT)) / 2) - 1)}} />);
  }
  return ticks;
});

function Rail({s}) {
  const x = Math.round(s.focus * RAIL / Math.max(s.order.length - 1, 1));
  return (
    <View className="absolute left-16 top-[440px] w-[1152px] h-[16px]">
      <View className="absolute left-0 right-0 top-[7px] h-[2px] bg-line" />
      <Ticks order={s.order} home={s.home} />
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
  if (was !== s.focus) notes.push(`antes em ${ordinal(was + 1)}`);
  if (s.focus >= HOME_LIMIT) notes.push('fora da tela inicial');
  const note = notes.join(', ');
  return (
    <>
      <Text className="absolute left-16 top-[354px] w-[1000px] font-inter text-title text-fg truncate">
        {game.nome}
      </Text>
      <View className="absolute left-16 bottom-[52px] flex-row items-end gap-6">
        <motion.View key={s.focus + 1} initial={{opacity: 0, x: 26 * s.dir}} animate={{opacity: 1, x: 0}}
          transition={{type: 'tween', duration: 0.2, ease: 'easeOut'}}>
          <Numeral value={s.focus + 1} />
        </motion.View>
        <View className="gap-1">
          <Text className="font-inter text-small text-subtle">{note || ' '}</Text>
          <Text className="font-inter text-of text-muted">de {s.order.length}</Text>
        </View>
      </View>
    </>
  );
}

function Header({name}) {
  return (
    <View className="absolute left-16 top-10 flex-row items-center gap-4">
      <Mark />
      <Text className="font-inter text-head text-fg">Fileira</Text>
      {name ? <Text className="font-inter text-head text-muted">{name}</Text> : null}
    </View>
  );
}

const ROW_HINTS = [['cross', 'Levantar'], ['triangle', 'Salvar'], ['options', 'Opções'], ['circle', 'Sair']];
const LIFT_HINTS = [['dpad', 'Mover'], ['cross', 'Soltar'], ['circle', 'Cancelar']];
const PICK_HINTS = [['cross', 'Escolher'], ['circle', 'Voltar']];

const START = {phase: 'loading', step: 'Procurando o serviço', error: null, profiles: [], user: null,
  games: {}, home: {}, base: [], order: [], focus: 0, dir: 1, lifted: false, liftFrom: 0, layer: null,
  target: null, toast: null, pending: null};

function App() {
  const [s, setS] = useState(START);
  const ref = useRef(s);
  ref.current = s;
  const patch = useCallback(next => setS(previous => ({...previous, ...(typeof next === 'function' ? next(previous) : next)})), []);
  const {focus} = useFocusActions();
  useInterfaceSounds();

  const dirty = s.phase === 'row' && changed(s) > 0;
  useLayoutEffect(() => {
    globalThis.__fileiraState = {phase: s.phase, focus: s.focus, lifted: s.lifted, dirty, count: s.order.length,
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

  const loadRow = async (user, profiles) => {
    const jogos = await api.row(user);
    if (!jogos.length) throw new api.Problem('A fileira está vazia', 'Este perfil não tem jogos na tela inicial.');
    const games = {};
    const home = {};
    jogos.forEach((game, i) => {
      games[game.titleId] = game;
      home[game.titleId] = i;
    });
    const base = jogos.map(game => game.titleId);
    light(false);
    patch({phase: 'row', profiles, user, games, home, base, order: base, focus: 0, dir: 1, lifted: false,
      layer: null, target: null, pending: {index: 0}});
  };

  const boot = async () => {
    patch({...START, phase: 'loading'});
    try {
      await api.ensureService(step => patch({step}));
      const profiles = await api.profiles();
      if (!profiles.length) {
        throw new api.Problem('Nenhum perfil encontrado', 'O serviço não achou perfis com fileira na tela inicial.');
      }
      const foreground = Users.getForeground();
      const id = foreground ? String(foreground.id >>> 0).padStart(10, '0') : '';
      await loadRow((profiles.find(profile => profile.userId === id) ?? profiles[0]).userId, profiles);
    } catch (error) {
      patch({phase: 'error', error: error instanceof api.Problem ? error
        : {title: 'Algo deu errado ao abrir', detail: String(error?.message ?? error)}});
    }
  };
  useEffect(() => {
    boot();
  }, []);

  const switchTo = async user => {
    patch({layer: 'busy'});
    try {
      await loadRow(user, ref.current.profiles);
    } catch (error) {
      close();
      toast(error.detail ?? error.message);
    }
  };

  // Saves, then shows the saved screen, or `next()` instead when switching profiles.
  const save = async next => {
    const now = ref.current;
    if (!changed(now)) {
      close();
      toast('A ordem já está salva.');
      return;
    }
    light(false);
    patch({lifted: false, layer: 'busy'});
    const result = await api.save(now.user, now.order, now.games);
    if (result.ok) {
      if (next) {
        toast('Ordem salva.');
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
  const onAction = action => {
    if (action === 'options') {
      light(false);
      patch({lifted: false, layer: 'menu'});
      return true;
    }
    if (action === 'triangle') {
      save();
      return true;
    }
    return false;
  };

  const discard = () => {
    patch(p => ({order: p.base, layer: null, pending: {index: 0}}));
    toast('Mudanças descartadas.');
  };
  const sortAZ = () => patch(p => {
    const id = p.order[p.focus];
    const order = p.order.slice().sort((a, b) => {
      const x = sortKey(p.games[a].nome);
      const y = sortKey(p.games[b].nome);
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
          <Text className="font-inter text-big text-fg">Fileira</Text>
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
        <Header name={profile?.nome} />
        <FocusScope autoFocus onBack={back} onAction={() => true}>
          {saved ? (
            <Message icon={<Badge kind="ok" />} title="Ordem salva"
              body="Fechando o app. A tela inicial já volta com a nova ordem.">
              <Button label="Fechar agora" autoFocus onPress={exit} />
            </Message>
          ) : (
            <Message icon={<Badge kind="error" />} title={s.error.title} body={s.error.detail}>
              <Button label="Tentar de novo" autoFocus onPress={boot} />
              <Button label="Fechar app" onPress={exit} />
            </Message>
          )}
        </FocusScope>
        <Hints items={[['cross', 'Escolher'], ['circle', 'Fechar']]} />
      </View>
    );
  }

  const count = changed(s);
  const hints = s.layer ? PICK_HINTS : s.lifted ? LIFT_HINTS : ROW_HINTS;
  return (
    <View className="w-screen h-screen bg-canvas">
      <Wash />
      <Header name={profile?.nome} />
      <FocusScope row autoFocus onBack={onBack} onAction={onAction}>
        <Slots count={s.order.length} onFocusIndex={onFocusIndex} onPressIndex={onPressIndex} />
      </FocusScope>
      <Row s={s} />
      <Rail s={s} />
      <Position s={s} />
      {s.layer === 'menu' || s.layer === 'profiles' ? (
        // One sheet for both pages, so it stays put and still knows which game to give focus back to.
        <Sheet page={s.layer} onClose={close}
          onBack={() => (s.layer === 'profiles' ? patch({layer: 'menu'}) : close())}
          title={s.layer === 'menu' ? 'Opções' : 'Trocar perfil'}
          subtitle={s.layer === 'menu' ? profile?.nome : 'Cada perfil tem a sua fileira'}>
          {s.layer === 'menu' ? [
            <SheetItem key="save" label="Salvar ordem" onPress={() => save()}
              detail={count ? `${count} ${count === 1 ? 'mudança' : 'mudanças'}` : 'sem mudanças'} />,
            <SheetItem key="discard" label="Descartar mudanças" onPress={discard} />,
            <SheetItem key="sort" label="Ordenar de A a Z" onPress={sortAZ} />,
            <SheetRule key="rule" />,
            <SheetItem key="profiles" label="Trocar perfil" onPress={() => patch({layer: 'profiles'})}
              detail={s.profiles.length > 1 ? `${s.profiles.length} perfis` : ''} />,
          ] : s.profiles.map(item => (
            <SheetItem key={item.userId} label={item.nome} checked={item.userId === s.user}
              autoFocus={item.userId === s.user} onPress={() => pickProfile(item.userId)} />
          ))}
        </Sheet>
      ) : null}
      {s.layer === 'exit' ? (
        <Dialog title="Salvar a nova ordem antes de sair?" onBack={close}
          body={count === 1 ? 'Um jogo mudou de lugar.' : `${count} jogos mudaram de lugar.`}>
          <Button label="Salvar e sair" onPress={() => save()} />
          <Button label="Sair sem salvar" onPress={exit} />
          <Button label="Voltar" onPress={close} />
        </Dialog>
      ) : null}
      {s.layer === 'switch' ? (
        <Dialog title="Trocar de perfil?" body="A nova ordem deste perfil ainda não foi salva."
          onBack={() => patch({layer: 'profiles'})}>
          <Button label="Salvar e trocar" onPress={() => save(() => switchTo(s.target))} />
          <Button label="Trocar sem salvar" onPress={() => switchTo(s.target)} />
          <Button label="Voltar" onPress={() => patch({layer: 'profiles'})} />
        </Dialog>
      ) : null}
      {s.layer === 'busy' ? (
        <FocusScope trap autoFocus onBack={() => true} onAction={() => true}>
          <View className="absolute inset-0 bg-black/70 items-center justify-center">
            <View focusable className="flex-row items-center gap-4 rounded-2xl bg-surface border-2 border-line px-8 py-6">
              <Mark breathing />
              <Text className="font-inter text-body text-fg">Um momento</Text>
            </View>
          </View>
        </FocusScope>
      ) : null}
      <Hints items={hints} />
      <AnimatePresence>{s.toast ? <Toast key={s.toast.at} text={s.toast.text} /> : null}</AnimatePresence>
    </View>
  );
}

AppRegistry.registerComponent('fileira', () => App);
