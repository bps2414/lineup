// SPDX-License-Identifier: GPL-3.0-or-later
// Interface text in English and Brazilian Portuguese, picked once from the console's system language:
// Portuguese for any pt-* language, English for everything else.
import {DeviceInfo} from '@ps5-react/core';

const englishOrdinal = n => {
  const tens = n % 100;
  const suffix = tens >= 11 && tens <= 13 ? 'th' : ['th', 'st', 'nd', 'rd'][n % 10] ?? 'th';
  return `${n}${suffix}`;
};

const en = {
  service: {
    looking: 'Looking for the service',
    replacing: 'Replacing the service with this version',
    starting: 'Starting the service',
    noApi: ['This build cannot start the service',
      'ps5-react lacks Payloads.start. Build the app again with the current framework.'],
    notStarted: error => ['The service did not start',
      `The ELF loader did not accept lineupd.elf (${error}). Check that it is running on port 9021.`],
    noAnswer: ['The service did not answer',
      'lineupd.elf was sent but did not answer within 5 seconds. Try again in a moment.'],
    stopped: what => ['The service stopped answering', `Could not read ${what}. Try again.`],
    readFailed: what => `Could not read ${what}`,
    status: code => `The service answered ${code}.`,
    profiles: 'the profiles',
    row: 'the row',
    saveNoAnswer: 'The service did not answer while saving. Open the app again to check the order.',
    saveStatus: code => `The service answered ${code}. Nothing was saved.`,
    stale: 'The row changed since it was read. It has been loaded again.',
  },
  emptyRow: ['The row is empty', 'This profile has no games on the home screen.'],
  noProfiles: ['No profiles found', 'The service found no profiles with a home-screen row.'],
  openFailed: 'Something went wrong while opening',
  before: n => `was ${englishOrdinal(n)}`,
  offHome: 'not on the home screen',
  of: n => `of ${n}`,
  hints: {
    lift: 'Lift', save: 'Save', options: 'Options', quit: 'Quit', move: 'Move', drop: 'Drop',
    cancel: 'Cancel', choose: 'Choose', back: 'Back', close: 'Close',
  },
  saved: ['Order saved', 'Closing the app. The home screen comes back with the new order.'],
  closeNow: 'Close now',
  retry: 'Try again',
  closeApp: 'Close app',
  alreadySaved: 'The order is already saved.',
  savedToast: 'Order saved.',
  discarded: 'Changes discarded.',
  menu: {
    title: 'Options', save: 'Save order', discard: 'Discard changes', sort: 'Sort A to Z',
    profiles: 'Switch profile',
    changes: n => (n ? `${n} ${n === 1 ? 'change' : 'changes'}` : 'no changes'),
    profileCount: n => `${n} profiles`,
    updates: 'Updates', updateTo: v => `Update to ${v}`,
  },
  updates: {
    title: 'Updates', checkOnOpen: 'Check when opening', checkNow: 'Check now',
    rollback: 'Go back to the previous version',
  },
  update: {
    available: v => `Lineup ${v} is available.`,
    upToDate: 'Lineup is up to date.',
    title: v => `Lineup ${v}`,
    size: bytes => `Download: ${(bytes / 1048576).toFixed(1)} MB`,
    install: 'Update', later: 'Not now',
    preparing: 'Getting ready',
    downloading: n => `Downloading the update, ${n}%`,
    extracting: 'Unpacking the update',
    applying: 'Closing to update',
    offline: 'Could not check for updates. Check the connection and try again.',
    checkStatus: code => `Could not check for updates. GitHub answered ${code}.`,
    badManifest: 'The update information is incomplete. Try again later.',
    noAnswer: 'The service did not answer. Open the app again and try again.',
    noInstall: detail => `Could not update: ${detail}`,
    downloadFailed: detail => `The download failed (${detail}). Try again to resume it.`,
    extractFailed: detail => `Could not unpack the update (${detail}).`,
    badPackage: 'The download is not a Lineup release. Nothing changed.',
    applyFailed: detail => `Could not start the update (${detail}). Nothing changed.`,
    failed: detail => `The update did not finish: ${detail}`,
    done: v => `Updated to ${v}.`,
    rollbackTitle: v => `Go back to ${v}?`,
    rollbackBody: 'The app closes and opens again on the previous version.',
    rollbackConfirm: 'Go back', cancel: 'Cancel',
  },
  profilesTitle: 'Switch profile',
  profilesSubtitle: 'Each profile has its own row',
  exitDialog: {
    title: 'Save the new order before leaving?',
    body: n => (n === 1 ? 'One game moved.' : `${n} games moved.`),
    save: 'Save and quit', discard: 'Quit without saving', back: 'Back',
  },
  switchDialog: {
    title: 'Switch profile?', body: "This profile's new order is not saved yet.",
    save: 'Save and switch', discard: 'Switch without saving', back: 'Back',
  },
  busy: 'One moment',
};

const pt = {
  service: {
    looking: 'Procurando o serviço',
    replacing: 'Trocando o serviço por esta versão',
    starting: 'Abrindo o serviço',
    noApi: ['Este build não abre o serviço',
      'Falta a chamada Payloads.start no ps5-react. Gere o app de novo com a versão atual do framework.'],
    notStarted: error => ['O serviço não abriu',
      `O ELF loader não aceitou o lineupd.elf (${error}). Confira se ele está ativo na porta 9021.`],
    noAnswer: ['O serviço não respondeu',
      'O lineupd.elf foi enviado, mas não respondeu em 5 segundos. Tente de novo em alguns instantes.'],
    stopped: what => ['O serviço parou de responder', `Não consegui ler ${what}. Tente de novo.`],
    readFailed: what => `Não consegui ler ${what}`,
    status: code => `O serviço respondeu ${code}.`,
    profiles: 'os perfis',
    row: 'a fileira',
    saveNoAnswer: 'O serviço não respondeu ao salvar. Abra o app de novo para conferir a ordem.',
    saveStatus: code => `O serviço respondeu ${code}. Nada foi gravado.`,
    stale: 'A fileira mudou desde que foi lida. Ela foi carregada de novo.',
  },
  emptyRow: ['A fileira está vazia', 'Este perfil não tem jogos na tela inicial.'],
  noProfiles: ['Nenhum perfil encontrado', 'O serviço não achou perfis com fileira na tela inicial.'],
  openFailed: 'Algo deu errado ao abrir',
  before: n => `antes em ${n}º`,
  offHome: 'fora da tela inicial',
  of: n => `de ${n}`,
  hints: {
    lift: 'Levantar', save: 'Salvar', options: 'Opções', quit: 'Sair', move: 'Mover', drop: 'Soltar',
    cancel: 'Cancelar', choose: 'Escolher', back: 'Voltar', close: 'Fechar',
  },
  saved: ['Ordem salva', 'Fechando o app. A tela inicial já volta com a nova ordem.'],
  closeNow: 'Fechar agora',
  retry: 'Tentar de novo',
  closeApp: 'Fechar app',
  alreadySaved: 'A ordem já está salva.',
  savedToast: 'Ordem salva.',
  discarded: 'Mudanças descartadas.',
  menu: {
    title: 'Opções', save: 'Salvar ordem', discard: 'Descartar mudanças', sort: 'Ordenar de A a Z',
    profiles: 'Trocar perfil',
    changes: n => (n ? `${n} ${n === 1 ? 'mudança' : 'mudanças'}` : 'sem mudanças'),
    profileCount: n => `${n} perfis`,
    updates: 'Atualizações', updateTo: v => `Atualizar para ${v}`,
  },
  updates: {
    title: 'Atualizações', checkOnOpen: 'Procurar ao abrir', checkNow: 'Procurar agora',
    rollback: 'Voltar à versão anterior',
  },
  update: {
    available: v => `Lineup ${v} disponível.`,
    upToDate: 'O Lineup está atualizado.',
    title: v => `Lineup ${v}`,
    size: bytes => `Download: ${(bytes / 1048576).toFixed(1).replace('.', ',')} MB`,
    install: 'Atualizar', later: 'Agora não',
    preparing: 'Preparando',
    downloading: n => `Baixando a atualização, ${n}%`,
    extracting: 'Abrindo a atualização',
    applying: 'Fechando para atualizar',
    offline: 'Não consegui procurar atualizações. Confira a conexão e tente de novo.',
    checkStatus: code => `Não consegui procurar atualizações. O GitHub respondeu ${code}.`,
    badManifest: 'As informações da atualização estão incompletas. Tente mais tarde.',
    noAnswer: 'O serviço não respondeu. Abra o app de novo e tente outra vez.',
    noInstall: detail => `Não deu para atualizar: ${detail}`,
    downloadFailed: detail => `O download falhou (${detail}). Tente de novo para continuar de onde parou.`,
    extractFailed: detail => `Não consegui abrir a atualização (${detail}).`,
    badPackage: 'O arquivo baixado não é uma versão do Lineup. Nada mudou.',
    applyFailed: detail => `Não consegui começar a atualização (${detail}). Nada mudou.`,
    failed: detail => `A atualização não terminou: ${detail}`,
    done: v => `Atualizado para ${v}.`,
    rollbackTitle: v => `Voltar para a ${v}?`,
    rollbackBody: 'O app fecha e abre de novo na versão anterior.',
    rollbackConfirm: 'Voltar', cancel: 'Cancelar',
  },
  profilesTitle: 'Trocar perfil',
  profilesSubtitle: 'Cada perfil tem a sua fileira',
  exitDialog: {
    title: 'Salvar a nova ordem antes de sair?',
    body: n => (n === 1 ? 'Um jogo mudou de lugar.' : `${n} jogos mudaram de lugar.`),
    save: 'Salvar e sair', discard: 'Sair sem salvar', back: 'Voltar',
  },
  switchDialog: {
    title: 'Trocar de perfil?', body: 'A nova ordem deste perfil ainda não foi salva.',
    save: 'Salvar e trocar', discard: 'Trocar sem salvar', back: 'Voltar',
  },
  busy: 'Um momento',
};

function systemLanguage() {
  try {
    return DeviceInfo.get().language ?? '';
  } catch {
    return '';
  }
}

/** The interface language as the update notes are keyed: 'pt-BR' or 'en'. */
export const LANGUAGE = systemLanguage().toLowerCase().startsWith('pt') ? 'pt-BR' : 'en';
export const T = LANGUAGE === 'pt-BR' ? pt : en;
