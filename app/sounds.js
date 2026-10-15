// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Interface sounds, from apps/starter. The WAVs are derived from Google's Material Design sound
// resources (CC BY 4.0; see assets/sounds/NOTICE.txt). Fileira only buzzes at the row's ends: Up and
// Down have nowhere to go on the main screen and stay silent.
import {Sound, useNavigationEvents} from '@ps5-react/core';
import focus from './assets/sounds/focus.wav';
import confirm from './assets/sounds/confirm.wav';
import back from './assets/sounds/back.wav';
import error from './assets/sounds/error.wav';

export {error};

export function useInterfaceSounds() {
  useNavigationEvents(event => {
    if (event.type === 'move') Sound.play(focus);
    else if (event.type === 'blocked') {
      if (event.direction === 'left' || event.direction === 'right') Sound.play(error);
    } else if (event.type === 'press') Sound.play(event.handled ? confirm : error);
    else if (event.handled && event.type === 'back') Sound.play(back);
    else if (event.handled) Sound.play(confirm);
  });
}
