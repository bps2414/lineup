// SPDX-License-Identifier: GPL-3.0-or-later
// Talks to fileirad (payloads/fileirad.c) on 127.0.0.1:9077 and starts it when it is not running.
import {Http, Payloads} from '@ps5-react/core';

export const VERSION = '1.0.1';
const BASE = 'http://127.0.0.1:9077';

/** An error the app shows as is: a short title and what to do about it. */
export class Problem extends Error {
  constructor(title, detail) {
    super(title);
    this.title = title;
    this.detail = detail;
  }
}

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
      headers: method === 'POST' ? {'X-Fileira': '1', 'Content-Type': 'text/plain; charset=utf-8'} : undefined,
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
    const {ok, data} = await call('/versao', {timeoutMs: 1500});
    return ok ? data.versao ?? '' : '';
  } catch {
    return null;
  }
}

/** Makes sure this app's version of the service answers, starting (or replacing) it when needed. */
export async function ensureService(onStep) {
  let running = await version();
  if (running === VERSION) return;
  if (running !== null) {
    onStep('Trocando o serviço por esta versão');
    await call('/sair', {method: 'POST', timeoutMs: 1500}).catch(() => {});
    await sleep(400);
  }
  onStep('Abrindo o serviço');
  if (typeof Payloads?.start !== 'function') {
    throw new Problem('Este build não abre o serviço',
      'Falta a chamada Payloads.start no ps5-react. Gere o app de novo com a versão atual do framework.');
  }
  const started = Payloads.start('fileirad.elf');
  if (!started.ok) {
    throw new Problem('O serviço não abriu',
      `O ELF loader não aceitou o fileirad.elf (${started.error}). Confira se ele está ativo na porta 9021.`);
  }
  for (let waited = 0; waited < 5000; waited += 250) {
    await sleep(250);
    running = await version();
    if (running === VERSION) return;
  }
  throw new Problem('O serviço não respondeu',
    'O fileirad.elf foi enviado, mas não respondeu em 5 segundos. Tente de novo em alguns instantes.');
}

async function read(path, what) {
  let reply;
  try {
    reply = await call(path, {timeoutMs: 8000});
  } catch {
    throw new Problem('O serviço parou de responder', `Não consegui ler ${what}. Tente de novo.`);
  }
  if (!reply.ok) throw new Problem(`Não consegui ler ${what}`, reply.data.error ?? `O serviço respondeu ${reply.status}.`);
  return reply.data;
}

export const profiles = async () => (await read('/perfis', 'os perfis')).perfis ?? [];
export const row = async user => (await read(`/fileira?user=${user}`, 'a fileira')).jogos ?? [];
export const iconUrl = game => `${BASE}/icone/${game.titleId}?m=${game.icone}`;

/**
 * Saves `order` (titleIds) for `user`, sending each game's index as it was read. Resolves to
 * {ok: true} or {ok: false, stale, message}; `stale` means the row changed since it was read.
 */
export async function save(user, order, games) {
  const body = `FIL1\n${order.map(id => `${id} ${games[id].indice}\n`).join('')}`;
  let reply;
  try {
    reply = await call(`/fileira?user=${user}`, {method: 'POST', body});
  } catch {
    return {ok: false, stale: false,
      message: 'O serviço não respondeu ao salvar. Abra o app de novo para conferir a ordem.'};
  }
  if (reply.ok) return {ok: true};
  return {ok: false, stale: reply.status === 409,
    message: reply.data.error ?? `O serviço respondeu ${reply.status}. Nada foi gravado.`};
}
