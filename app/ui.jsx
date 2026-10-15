// SPDX-License-Identifier: GPL-3.0-or-later
// Lineup's interface pieces: the mark, profile pictures, controller glyphs, button hints, the lift light, the pin
// badge, the side sheet, dialogs, buttons, full-screen messages and the toast. Black and white only.
import {useEffect} from 'react';
import {Animated, Circle, Easing, FocusScope, Image, Path, Rect, Svg, Text, View, motion, transitions,
  useAnimatedValue, useIsFocused} from '@ps5-react/core';
import {avatarUrl} from './api.js';

export const C = {canvas: '#000000', surface: '#17171a', surface2: '#212125', line: '#2a2a2f',
  fg: '#f2f2f3', muted: '#a3a3ab', subtle: '#8a8a94', dim: '#55555c'};

// Raw style numbers are render pixels; classes and motion values are logical (1280 wide).
const SCALE = screen.width / 1280;
export const px = n => Math.round(n * SCALE);

const slide = {type: 'tween', duration: 0.26, ease: [0.22, 1, 0.36, 1]};

/** Four tiles, the second lifted: the app's mark, as on its icon. */
export function Mark({breathing = false}) {
  return (
    <View className="flex-row items-end gap-[3px] h-[18px]">
      <View className="size-[9px] rounded-[2px] bg-fg" />
      {breathing ? (
        <motion.View className="size-[9px] rounded-[2px] bg-fg" initial={{y: 0}} animate={{y: -6}}
          transition={{type: 'tween', duration: 0.7, ease: 'easeInOut', repeat: Infinity, repeatType: 'reverse'}} />
      ) : <View className="size-[9px] rounded-[2px] bg-fg -translate-y-[6px]" />}
      <View className="size-[9px] rounded-[2px] bg-fg" />
      <View className="size-[9px] rounded-[2px] bg-fg" />
    </View>
  );
}

// Glyphs are drawn on a 24-unit grid: a hairline outline for the button, the symbol in full white.
const OUTLINE = {fill: 'none', stroke: C.dim, strokeWidth: 1.5};
const MARK = {fill: 'none', stroke: C.fg, strokeWidth: 2, strokeLinecap: 'round', strokeLinejoin: 'round'};

const SHAPES = {
  cross: () => <Path d="M8.5 8.5 15.5 15.5M15.5 8.5 8.5 15.5" {...MARK} />,
  circle: () => <Circle cx={12} cy={12} r={4.25} {...MARK} />,
  triangle: () => <Path d="M12 7.25 16.5 15.25H7.5Z" {...MARK} />,
  square: () => <Rect x={8} y={8} width={8} height={8} {...MARK} />,
};

export function Glyph({name, size = 22}) {
  if (name === 'options') {
    return (
      <Svg width={px(size * 1.4)} height={px(size)} viewBox="0 0 34 24">
        <Rect x={1} y={3.5} width={32} height={17} rx={8.5} {...OUTLINE} />
        <Path d="M12 9h10M12 12h10M12 15h10" {...MARK} strokeWidth={1.6} />
      </Svg>
    );
  }
  if (name === 'dpad') {
    return (
      <Svg width={px(size)} height={px(size)} viewBox="0 0 24 24">
        <Path d="M9 2.5h6v6.5h6.5v6H15v6.5H9V15H2.5V9H9Z" {...OUTLINE} />
        <Rect x={3.5} y={10} width={5} height={4} rx={1} fill={C.fg} />
        <Rect x={15.5} y={10} width={5} height={4} rx={1} fill={C.fg} />
      </Svg>
    );
  }
  // Shapes are called, not rendered as components: an Svg only draws its host children.
  return (
    <Svg width={px(size)} height={px(size)} viewBox="0 0 24 24">
      <Circle cx={12} cy={12} r={10.75} {...OUTLINE} />
      {SHAPES[name]()}
    </Svg>
  );
}

/** Bottom-right legend: [glyph, label] pairs. */
export function Hints({items}) {
  return (
    <View className="absolute right-16 bottom-9 flex-row items-center gap-7">
      {items.map(([glyph, label]) => (
        <View key={glyph} className="flex-row items-center gap-2.5">
          <Glyph name={glyph} />
          <Text className="font-inter text-hint text-muted">{label}</Text>
        </View>
      ))}
    </View>
  );
}

/** A light that runs around a lifted game, over a faint ring that keeps the shape when the light is away. */
export function LiftLight({radius}) {
  const phase = useAnimatedValue(0);
  useEffect(() => {
    const run = Animated.loop(Animated.timing(phase, {toValue: 1, duration: 1800, easing: Easing.linear}));
    run.start();
    return () => run.stop();
  }, []);
  const fill = {position: 'absolute', left: 0, top: 0, right: 0, bottom: 0, borderRadius: px(radius)};
  return (
    <>
      <View pointerEvents="none" style={[fill, {borderWidth: px(2), borderColor: '#ffffff40'}]} />
      <Animated.View pointerEvents="none" style={[fill, {borderSweepColor: '#ffffff', borderSweepWidth: px(3),
        borderSweepLength: 0.45, borderSweepPhase: phase}]} />
    </>
  );
}

/** A pushpin on a dark disc, in a pinned game's corner: the 168 px icon's when `big`, else the 128 px one's. */
export function PinBadge({big = false}) {
  const size = big ? 30 : 24;
  return (
    <View className={big ? 'absolute right-4 top-4 rounded-full bg-black/70 items-center justify-center'
      : 'absolute right-1.5 top-1.5 rounded-full bg-black/70 items-center justify-center'}
      style={{width: px(size), height: px(size)}}>
      <Svg width={px(size * 0.7)} height={px(size * 0.7)} viewBox="0 0 24 24">
        <Path d="M8.5 3.5h7M10 3.5v6l-3 3.5h10l-3-3.5v-6M12 13v7.5" {...MARK} />
      </Svg>
    </View>
  );
}

export function Check({size = 20, color = C.fg}) {
  return (
    <Svg width={px(size)} height={px(size)} viewBox="0 0 24 24">
      <Path d="M5 12.5 10 17.5 19 7" fill="none" stroke={color} strokeWidth={2.4} strokeLinecap="round"
        strokeLinejoin="round" />
    </Svg>
  );
}

function ItemBody({label, detail, checked}) {
  const focused = useIsFocused();
  return (
    <>
      <Text focused={focused} className="flex-1 font-inter text-body text-fg focused:text-canvas truncate">
        {label}
      </Text>
      {detail ? (
        <Text focused={focused} className="font-inter text-small text-subtle focused:text-[#4a4a52]">{detail}</Text>
      ) : null}
      {checked ? <Check color={focused ? C.canvas : C.fg} /> : null}
    </>
  );
}

// Greys only, to keep the app black and white; a name always gets the same one.
const SHADES = ['#2a2a2f', '#35353b', '#414148', '#4d4d55', '#5a5a63'];

/** A profile's picture, over its initial on a grey picked from the name, which shows while there is none. */
export function Avatar({profile, size}) {
  const name = profile?.name ?? '';
  let hash = 0;
  for (const c of name) hash = (hash * 31 + c.codePointAt(0)) | 0;
  return (
    <View className="rounded-full items-center justify-center"
      style={{width: px(size), height: px(size), backgroundColor: SHADES[Math.abs(hash) % SHADES.length]}}>
      <Text className="font-inter text-fg" style={{fontSize: px(size * 0.45)}}>{[...name][0]?.toUpperCase() ?? ''}</Text>
      {profile?.avatar ? <Image source={{uri: avatarUrl(profile)}} className="absolute inset-0 rounded-full" /> : null}
    </View>
  );
}

/** A sheet row: white text on the panel, inverted while focused. `leading` goes left of the label. */
export function SheetItem({label, detail, leading, checked = false, autoFocus = false, onPress}) {
  return (
    <View onPress={onPress} autoFocus={autoFocus}
      className="flex-row items-center gap-4 h-[56px] px-5 rounded-[10px] focused:bg-fg">
      {leading}
      <ItemBody label={label} detail={detail} checked={checked} />
    </View>
  );
}

export const SheetRule = () => <View className="h-[2px] mx-5 my-2 bg-line" />;

/**
 * Right side sheet. Traps focus; Circle and Options close it. `page` remounts the list (and its focus).
 * `leading` goes left of the subtitle.
 */
export function Sheet({title, subtitle, leading, page, onClose, onBack = onClose, children}) {
  const back = () => {
    onBack();
    return true;
  };
  const action = name => {
    if (name === 'options') onClose();
    return true;
  };
  return (
    <FocusScope trap autoFocus onBack={back} onAction={action}>
      <View className="absolute inset-0">
        <View className="absolute inset-0 bg-black/70" />
        <motion.View className="absolute right-0 top-0 bottom-0 w-[440px] bg-surface border-l-2 border-line
          px-5 pt-14 pb-24 gap-8" initial={{x: 440}} animate={{x: 0}} transition={slide}>
          <View className="px-5 gap-1.5">
            <Text className="font-inter text-sheet text-fg">{title}</Text>
            {subtitle ? (
              <View className="flex-row items-center gap-2.5">
                {leading}
                <Text className="flex-1 font-inter text-small text-muted truncate">{subtitle}</Text>
              </View>
            ) : null}
          </View>
          <FocusScope key={page} autoFocus>
            <View className="gap-1">{children}</View>
          </FocusScope>
        </motion.View>
      </View>
    </FocusScope>
  );
}

function ButtonLabel({children}) {
  const focused = useIsFocused();
  return <Text focused={focused} className="font-inter text-body text-fg focused:text-canvas">{children}</Text>;
}

export function Button({label, onPress, autoFocus = false}) {
  return (
    <View onPress={onPress} autoFocus={autoFocus}
      className="h-[54px] px-7 rounded-[10px] bg-surface2 justify-center items-center focused:bg-fg">
      <ButtonLabel>{label}</ButtonLabel>
    </View>
  );
}

/** Centered question over the screen; buttons stack, the first takes focus. */
// `wide` is for long text (update notes): wider, and cut with … so the dialog never outgrows the 720 px screen.
export function Dialog({title, body, wide, onBack, children}) {
  const back = () => {
    onBack();
    return true;
  };
  return (
    <FocusScope trap autoFocus onBack={back} onAction={() => true}>
      <View className="absolute inset-0 items-center justify-center">
        <View className="absolute inset-0 bg-black/80" />
        <motion.View className={`${wide ? 'w-[760px]' : 'w-[520px]'} rounded-2xl bg-surface border-2 border-line p-8 gap-7`}
          initial={{opacity: 0, y: 14}} animate={{opacity: 1, y: 0}} transition={slide}>
          <View className="gap-2.5">
            <Text className="font-inter text-sheet text-fg">{title}</Text>
            {body ? (
              <Text className="font-inter text-body text-muted" numberOfLines={wide ? 13 : undefined}>{body}</Text>
            ) : null}
          </View>
          <View className="gap-2">{children}</View>
        </motion.View>
      </View>
    </FocusScope>
  );
}

/** A whole-screen state (loading, error, saved): left aligned on the margin, like the row. */
export function Message({icon, title, body, children}) {
  return (
    <View className="absolute left-16 top-[208px] w-[760px] gap-6">
      {icon}
      <View className="gap-3">
        <Text className="font-inter text-big text-fg">{title}</Text>
        {body ? <Text className="font-inter text-body text-muted">{body}</Text> : null}
      </View>
      {children ? <View className="flex-row gap-3 pt-4">{children}</View> : null}
    </View>
  );
}

export function Badge({kind}) {
  return (
    <Svg width={px(64)} height={px(64)} viewBox="0 0 64 64">
      <Circle cx={32} cy={32} r={30} fill="none" stroke={C.fg} strokeWidth={2.5} />
      {kind === 'ok' ? (
        <Path d="M20 33 28.5 41.5 44.5 24" fill="none" stroke={C.fg} strokeWidth={3.5} strokeLinecap="round"
          strokeLinejoin="round" />
      ) : (
        <Path d="M32 18V36M32 45V46" fill="none" stroke={C.fg} strokeWidth={4} strokeLinecap="round" />
      )}
    </Svg>
  );
}

export function Toast({text}) {
  return (
    <motion.View className="absolute right-16 top-9 max-w-[560px] rounded-[10px] bg-fg px-5 py-3"
      initial={{opacity: 0, y: -10}} animate={{opacity: 1, y: 0}}
      exit={{opacity: 0, transition: transitions.exit}} transition={slide}>
      <Text className="font-inter text-small text-canvas">{text}</Text>
    </motion.View>
  );
}

/** The faint light in the top-left corner of every screen. */
export const Wash = () => (
  <View className="absolute -left-[560px] -top-[560px] w-[1120px] h-[1120px]" style={{backgroundGradient: {
    type: 'radial', stops: [{color: '#ffffff12', offset: 0}, {color: '#ffffff05', offset: 0.35},
      {color: '#ffffff00', offset: 0.7}]}}} />
);
