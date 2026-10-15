// SPDX-License-Identifier: GPL-3.0-or-later
// Talks to lineupd (payloads/lineupd.c) on 127.0.0.1:9077 and starts it when it is not running.
import {Archives, Downloads, FileSystem, Http, Payloads} from '@ps5-react/core';
import {T} from './i18n.js';

export const VERSION = '1.2.0';
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
      headers: method === 'POST' ? {'X-Lineup': '1', 'Content-Type': 'text/plain; charset=utf-8'} : {'X-Lineup': '1'},
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
/** {games, pins}: each game's `pinned` is its slot or null; `pins` is 'on', 'missing' (triggers lost) or 'off'. */
export async function row(user) {
  const data = await read(`/row?user=${user}`, T.service.row);
  return {games: data.games ?? [], pins: data.pins ?? 'off'};
}
export const iconUrl = game => `${BASE}/icon/${game.titleId}?m=${game.icon}`;
export const avatarUrl = profile => `${BASE}/avatar/${profile.userId}?m=${profile.avatar}`;

/**
 * Saves `order` (titleIds) for `user`, sending each game's index as it was read and pinning the games in `pins`
 * where they stand. Resolves to {ok: true} or {ok: false, stale, message}; `stale` means the row changed since it
 * was read.
 */
export async function save(user, order, games, pins) {
  const body = `LIN1\n${order.map(id => `${id} ${games[id].index}${pins[id] ? ' pin' : ''}\n`).join('')}`;
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

/** Removes every profile's pinned games and all Lineup added to the home screen database. Resolves to an error message or ''. */
export async function unpinAll() {
  try {
    const reply = await call('/pins/uninstall', {method: 'POST', timeoutMs: 15000});
    return reply.ok ? '' : T.pins.removeFailed(reply.data.error ?? T.service.status(reply.status));
  } catch {
    return T.pins.removeFailed(T.pins.noAnswer);
  }
}

// ---- Updates ----

const UPDATE_DIR = '/data/lineup/update';
const SETTINGS = '/data/lineup/settings.json';
const MANIFEST = 'https://github.com/bps2414/lineup/releases/latest/download/update.json';
const RELEASES = 'https://github.com/bps2414/lineup/releases/download';

// "1.2.0", or param.json's "01.002.000", as one comparable number; -1 when malformed.
const versionNumber = text => (/^\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(text ?? '')
  ? text.split('.').reduce((n, part) => n * 1000 + Number(part), 0) : -1);

export function settings() {
  try {
    return {checkOnOpen: true, lastCheck: 0, ...JSON.parse(FileSystem.readFile(SETTINGS))};
  } catch {
    return {checkOnOpen: true, lastCheck: 0};
  }
}

export function saveSettings(next) {
  try {
    FileSystem.mkdir('/data/lineup', {recursive: true});
    FileSystem.writeFile(SETTINGS, JSON.stringify(next));
  } catch {
    // Settings that do not stick are not worth stopping for.
  }
}

/** The release manifest when it offers a version newer than this one, else null. Throws when it cannot be read. */
export async function checkUpdate() {
  const limit = deadline(20000);
  let response;
  let manifest;
  try {
    response = await Http.request(MANIFEST, {maxBytes: 64 * 1024, signal: limit.signal});
    if (response.status === 404) return null;
    manifest = response.ok ? await response.json() : null;
  } catch {
    throw new Error(T.update.offline);
  } finally {
    limit.clear();
  }
  if (!manifest) throw new Error(T.update.checkStatus(response.status));
  // A version at or below this one is never offered: going back is only through the previous copy.
  return versionNumber(manifest.version) > versionNumber(VERSION) ? manifest : null;
}

/** How Lineup is installed: {kind: 'folder' | 'image', path, previous}. Throws with the service's detail. */
export async function install() {
  let reply;
  try {
    reply = await call('/update/install', {timeoutMs: 8000});
  } catch {
    throw new Error(T.update.noAnswer);
  }
  if (!reply.ok) throw new Error(T.update.noInstall(reply.data.error ?? T.service.status(reply.status)));
  return reply.data;
}

/**
 * Downloads `manifest`'s file for this kind of install into /data/lineup/update/<version>/ and, for a folder,
 * unpacks and checks it. `onStep(text)` reports progress.
 */
export async function download(manifest, kind, onStep) {
  const {version} = manifest;
  const file = kind === 'image' ? manifest.exfat : manifest.zip;
  if (versionNumber(version) < 0 || !file || !/^[\w.-]+$/.test(file.name ?? '') || !/^[0-9a-f]{64}$/i.test(file.sha256 ?? '')
      || !Number.isSafeInteger(file.size) || file.size <= 0) {
    throw new Error(T.update.badManifest);
  }
  const dir = `${UPDATE_DIR}/${version}`;
  // lineupd looks for exactly these names.
  const destination = `${dir}/Lineup-${version}.${kind === 'image' ? 'exfat' : 'zip'}`;
  FileSystem.mkdir(dir, {recursive: true});
  const task = Downloads.enqueue({url: `${RELEASES}/v${version}/${encodeURIComponent(file.name)}`, destination,
    sha256: file.sha256.toLowerCase(), expectedBytes: file.size, recoverCompleted: true});
  const stop = task.subscribe(({written}) => onStep(T.update.downloading(Math.floor(100 * written / file.size))));
  try {
    await task.done;
  } catch (error) {
    throw new Error(T.update.downloadFailed(error?.message ?? String(error)));
  } finally {
    stop();
  }
  if (kind === 'image') {
    if (Archives.inspect([destination]).kind !== 'exfat') throw new Error(T.update.badPackage);
    return;
  }
  onStep(T.update.extracting);
  const stage = `${dir}/stage`;
  try {
    await Archives.extract({sources: [destination], destination: stage, maxBytes: 1024 ** 3}).done;
  } catch (error) {
    throw new Error(T.update.extractFailed(error?.message ?? String(error)));
  }
  const app = `${stage}/PPSA99077`;
  let param = {};
  try {
    param = JSON.parse(FileSystem.readFile(`${app}/sce_sys/param.json`));
  } catch {
    // Checked below.
  }
  if (param.titleId !== 'PPSA99077' || versionNumber(param.contentVersion) !== versionNumber(version)
      || !FileSystem.exists(`${app}/eboot.bin`) || !FileSystem.exists(`${app}/lineupd.elf`)) {
    throw new Error(T.update.badPackage);
  }
}

/** Hands the swap to lineupd; `version` 'previous' goes back. The app must close right after. */
export async function apply(version) {
  let reply;
  try {
    reply = await call(`/update/apply?version=${version}`, {method: 'POST', timeoutMs: 8000});
  } catch {
    throw new Error(T.update.noAnswer);
  }
  if (!reply.ok) throw new Error(T.update.applyFailed(reply.data.error ?? T.service.status(reply.status)));
}

/** After an update: what to tell the user, once (and ok-<version> is written), or ''. */
export function updateOutcome() {
  try {
    const error = `${UPDATE_DIR}/error`;
    if (FileSystem.exists(error)) {
      const detail = FileSystem.readFile(error).trim();
      FileSystem.remove(error);
      return T.update.failed(detail);
    }
    const dir = `${UPDATE_DIR}/${VERSION}`;
    const ok = `${UPDATE_DIR}/ok-${VERSION}`;
    if (!FileSystem.exists(dir) || FileSystem.exists(ok)) return '';
    FileSystem.writeFile(ok, '');
    // The download and its unpacked copy are not needed any more.
    FileSystem.removeAsync(dir).catch(() => {});
    return T.update.done(VERSION);
  } catch {
    return '';
  }
}
