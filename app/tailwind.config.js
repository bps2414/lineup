// SPDX-License-Identifier: GPL-3.0-or-later
// Lineup is black and white only: one canvas, two raised surfaces, a hairline and three text greys.
export default {theme: {extend: {
  colors: {
    canvas: '#000000', surface: '#17171a', surface2: '#212125', line: '#2a2a2f',
    fg: '#f2f2f3', muted: '#a3a3ab', subtle: '#8a8a94', dim: '#55555c',
  },
  fontFamily: {inter: './assets/Inter-Regular.ttf'},
  fontSize: {
    hint: '16px', small: '18px', body: '20px', head: '22px', sheet: '26px', of: '30px', title: '34px',
    big: '40px',
  },
}}};
