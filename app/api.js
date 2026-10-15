// SPDX-License-Identifier: GPL-3.0-or-later
// Talks to lineupd (payloads/lineupd.c) on 127.0.0.1:9077 and starts it when it is not running.
import {Http, Payloads} from '@ps5-react/core';
import {T} from './i18n.js';

export const VERSION = '1.1.0';
const BASE = 'http://127.0.0.1:9077';

/** An error the app shows as is: a short title and what to do about it. */
export class Problem extends Error {
  constructor(title, detail) {
    super(title);
    this.title = title;
    this.detail = detail;
  }
}

const problem = ([title, detail]) => new Problem(title, detail);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// Http.request takes any object shaped like an AbortSignal; QuickJS has no AbortController.
function deadline(ms) {
  const listeners = new Set();
  const signal = {
    aborted: false,
    addEventListener: (_type, listener) => listeners.add(listener),
    removeEventListener: (_type, listener) => listeners.delete(listener),
  };
  const timer = setTimeout(() => {
    signal.aborted = true;
    for (const listener of [...listeners]) listener();
  }, ms);
  return {signal, clear: () => clearTimeout(timer)};
}

async function call(path, {method = 'GET', body, timeoutMs} = {}) {
  const limit = timeoutMs ? deadline(timeoutMs) : null;
  try {
    const response = await Http.request(BASE + path, {
      method, body, maxBytes: 1024 * 1024, signal: limit?.signal,
      headers: method === 'POST' ? {'X-Lineup': '1', 'Content-Type': 'text/plain; charset=utf-8'} : undefined,
    });
    let data = {};
    try {
      data = await response.json();
    } catch {
      // A body that is not JSON keeps the status alone.
    }
    return {status: response.status, ok: response.ok, data};
  } finally {
    limit?.clear();
  }
}

async function version() {
  try {
    const {ok, data} = await call('/version', {timeoutMs: 1500});
    return ok ? data.version ?? '' : '';
  } catch {
    return null;
  }
}

/** Makes sure this app's version of the service answers, starting (or replacing) it when needed. */
export async function ensureService(onStep) {
  let running = await version();
  if (running === VERSION) return;
  if (running !== null) {
    onStep(T.service.replacing);
    // A service from before the rename answers /versao and quits on /sair.
    await call('/quit', {method: 'POST', timeoutMs: 1500}).catch(() => {});
    await Http.request(`${BASE}/sair`, {method: 'POST', headers: {'X-Fileira': '1'}}).catch(() => {});
    await sleep(400);
  }
  onStep(T.service.starting);
  if (typeof Payloads?.start !== 'function') throw problem(T.service.noApi);
  const started = Payloads.start('lineupd.elf');
  if (!started.ok) throw problem(T.service.notStarted(started.error));
  for (let waited = 0; waited < 5000; waited += 250) {
    await sleep(250);
    running = await version();
    if (running === VERSION) return;
  }
  throw problem(T.service.noAnswer);
}

// The service's own message is English detail; the title is always in the interface language.
async function read(path, what) {
  let reply;
  try {
    reply = await call(path, {timeoutMs: 8000});
  } catch {
    throw problem(T.service.stopped(what));
  }
  if (!reply.ok) throw new Problem(T.service.readFailed(what), reply.data.error ?? T.service.status(reply.status));
  return reply.data;
}

export const profiles = async () => (await read('/profiles', T.service.profiles)).profiles ?? [];
export const row = async user => (await read(`/row?user=${user}`, T.service.row)).games ?? [];
export const iconUrl = game => `${BASE}/icon/${game.titleId}?m=${game.icon}`;

/**
 * Saves `order` (titleIds) for `user`, sending each game's index as it was read. Resolves to
 * {ok: true} or {ok: false, stale, message}; `stale` means the row changed since it was read.
 */
export async function save(user, order, games) {
  const body = `LIN1\n${order.map(id => `${id} ${games[id].index}\n`).join('')}`;
  let reply;
  try {
    reply = await call(`/row?user=${user}`, {method: 'POST', body});
  } catch {
    return {ok: false, stale: false, message: T.service.saveNoAnswer};
  }
  if (reply.ok) return {ok: true};
  if (reply.status === 409) return {ok: false, stale: true, message: T.service.stale};
  const detail = reply.data.error ? ` (${reply.data.error})` : '';
  return {ok: false, stale: false, message: T.service.saveStatus(reply.status) + detail};
}
