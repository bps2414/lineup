export const locales = ['en', 'pt-br'] as const;
export type Locale = (typeof locales)[number];

export const htmlLang: Record<Locale, string> = { en: 'en', 'pt-br': 'pt-BR' };

const REPO = 'https://github.com/bps2414/lineup';
export const links = {
  repo: REPO,
  latest: `${REPO}/releases/latest`,
  issues: `${REPO}/issues`,
  license: `${REPO}/blob/main/LICENSE`,
};

const en = {
  meta: {
    title: 'Lineup: arrange your PS5 home screen with the controller',
    description:
      'Homebrew for a jailbroken PS5 you own. Lift a game, carry it, drop it, and pin the ones that should never move. Backs up the home screen before every save.',
  },
  nav: { github: 'GitHub', language: 'Language' },
  hero: {
    title: 'Arrange your PS5 home screen with the controller.',
    lede: 'The console orders your row by what you opened last. Lineup lets you put every game where you want it, on the TV, and pin the ones that should never move.',
    download: 'Download the latest release',
    fine: 'Free and open source homebrew for a jailbroken PS5 you own. Tested on firmware 13.60.',
    source: 'Read the source on GitHub',
    caption: 'Lift a game, carry it, drop it, pin it. The rest of the row makes room.',
    keys: { lift: 'Lift', carry: 'Carry', drop: 'Drop', pin: 'Pin' },
  },
  why: {
    title: 'Your row, not the console’s',
    items: [
      { title: 'On the TV, with the controller', text: 'No PC and no editing databases over FTP. You see the row as it is and move games where you want them.' },
      { title: 'Pins that hold', text: 'A pinned game keeps its place when you open other games. Everything else still follows the system’s rule.' },
      { title: 'See where the edge is', text: 'The home screen shows only the first 12 games. A divider in the row shows where that edge falls.' },
      { title: 'Tidy the tabs', text: 'Move homebrew and emulators between Games and Media, or hide any app on the current profile.' },
    ],
    shot: 'Lineup with a game focused at position 13, just past the edge of the home screen.',
  },
  controls: {
    title: 'Four buttons',
    items: [
      { key: '✕', text: 'Lift the focused game, then drop it where you are.' },
      { key: '□', text: 'Pin or unpin it. Pinned games keep their place.' },
      { key: '△', text: 'Save. Lineup closes and the home screen comes back in the new order, no restart.' },
      { key: '◯', text: 'Put a carried game back, or quit. It asks before losing changes.' },
    ],
    more: 'L1 and R1 switch between the Games and Media tabs. Options opens the menu: sort A to Z, pin the whole row, hidden apps, profiles, updates.',
    shot: 'The Options menu.',
  },
  safety: {
    title: 'Careful with your console',
    items: [
      'Before each save, the home screen’s database is copied to a backup folder. The 5 newest copies are kept.',
      'The database is checked before the write and again after it, and the whole save is one transaction.',
      'The new order reuses the values the row already holds, so nothing grows past what the system expects.',
      'If the row changed since Lineup read it, the save is refused and the row reloads.',
    ],
  },
  install: {
    title: 'Install',
    needs: 'You need a PS5 running a jailbreak with kstuff, an ELF loader on port 9021 (elfldr, or etaHEN’s) and ShadowMountPlus.',
    items: [
      'Download Lineup-<version>.exfat from the latest release.',
      'Copy it into /data/homebrew/ over FTP, or into a homebrew/ folder on an exFAT USB drive.',
      'Wait a few seconds: ShadowMountPlus mounts it and adds Lineup to the home screen.',
    ],
    update: 'After that, Lineup checks for new releases when it opens and updates itself when you choose Update.',
  },
  faq: {
    title: 'Questions',
    items: [
      { q: 'A game I opened jumped back to the front.', a: 'That is the system’s rule, and Lineup keeps it. Pin the game with □ and save: it holds its place from then on.' },
      { q: 'Does the order survive a restart?', a: 'Yes. Lineup writes it where the home screen keeps the row, and hidden apps stay hidden too.' },
      { q: 'Does it work on my firmware?', a: 'It is tested on 13.60. If you try another version, open an issue saying whether it worked.' },
      { q: 'How do I go back to the system’s order?', a: 'Options → More options → Remove pins from every profile. Every profile goes back to “last opened comes first”, and hidden apps come back.' },
      { q: 'Something went wrong.', a: 'Options → More options → Save a bug report, then attach the file to an issue. It lists the apps and how the home screen stores them, with no names or passwords.' },
    ],
    issue: 'Open an issue',
  },
  footer: {
    star: 'If Lineup tidied your home screen, a star on GitHub helps other people find it.',
    starCta: 'Star on GitHub',
    disclaimer:
      'Lineup is homebrew for consoles you own. It only changes the order and visibility of apps already installed, and it does not include, download or unlock any game, firmware or key. It is an independent project, not affiliated with or endorsed by Sony Interactive Entertainment. “PlayStation” and “PS5” are trademarks of their owners. Use at your own risk.',
    credits: 'Built on PS5 React by half144 and its contributors.',
    license: 'GPL-3.0 license',
  },
};

type Strings = typeof en;

const pt: Strings = {
  meta: {
    title: 'Lineup: arrume a tela inicial do PS5 pelo controle',
    description:
      'Homebrew para um PS5 desbloqueado que é seu. Levante um jogo, leve, solte, e fixe os que nunca devem sair do lugar. Faz backup da tela inicial antes de cada gravação.',
  },
  nav: { github: 'GitHub', language: 'Idioma' },
  hero: {
    title: 'Arrume a tela inicial do PS5 pelo controle.',
    lede: 'O console ordena sua fileira pelo que você abriu por último. O Lineup deixa você pôr cada jogo onde quiser, na TV, e fixar os que nunca devem sair do lugar.',
    download: 'Baixar a última versão',
    fine: 'Homebrew grátis e de código aberto para um PS5 desbloqueado que é seu. Testado no firmware 13.60.',
    source: 'Ver o código no GitHub',
    caption: 'Levante um jogo, leve, solte, fixe. O resto da fileira abre espaço.',
    keys: { lift: 'Levantar', carry: 'Levar', drop: 'Soltar', pin: 'Fixar' },
  },
  why: {
    title: 'A fileira é sua, não do console',
    items: [
      { title: 'Na TV, pelo controle', text: 'Sem PC e sem editar banco de dados por FTP. Você vê a fileira como ela é e põe cada jogo onde quiser.' },
      { title: 'Fixos que ficam', text: 'Um jogo fixo mantém o lugar quando você abre outros jogos. O resto continua seguindo a regra do sistema.' },
      { title: 'Veja onde fica o limite', text: 'A tela inicial mostra só os 12 primeiros jogos. Uma divisória na fileira mostra onde esse limite cai.' },
      { title: 'Arrume as abas', text: 'Mova homebrew e emuladores entre Jogos e Mídia, ou esconda qualquer app no perfil atual.' },
    ],
    shot: 'O Lineup com um jogo em foco na 13ª posição, logo depois do limite da tela inicial.',
  },
  controls: {
    title: 'Quatro botões',
    items: [
      { key: '✕', text: 'Levanta o jogo em foco e depois solta onde você estiver.' },
      { key: '□', text: 'Fixa ou solta. Jogos fixos mantêm o lugar.' },
      { key: '△', text: 'Salva. O Lineup fecha e a tela inicial volta na nova ordem, sem reiniciar.' },
      { key: '◯', text: 'Devolve o jogo que você está levando, ou sai. Pergunta antes de perder mudanças.' },
    ],
    more: 'L1 e R1 trocam entre as abas Jogos e Mídia. Options abre o menu: ordem de A a Z, fixar a fileira toda, apps escondidos, perfis, atualizações.',
    shot: 'O menu Options.',
  },
  safety: {
    title: 'Cuidado com o seu console',
    items: [
      'Antes de cada gravação, o banco da tela inicial é copiado para uma pasta de backup. As 5 cópias mais novas ficam guardadas.',
      'O banco é conferido antes de gravar e de novo depois, e a gravação inteira é uma transação só.',
      'A nova ordem reaproveita os valores que a fileira já tem, então nada passa do que o sistema espera.',
      'Se a fileira mudou desde que o Lineup leu, a gravação é recusada e a fileira recarrega.',
    ],
  },
  install: {
    title: 'Instalar',
    needs: 'Você precisa de um PS5 com jailbreak, kstuff, um ELF loader na porta 9021 (elfldr, ou o do etaHEN) e o ShadowMountPlus.',
    items: [
      'Baixe o Lineup-<versão>.exfat da última release.',
      'Copie para /data/homebrew/ por FTP, ou para uma pasta homebrew/ num pendrive exFAT.',
      'Espere alguns segundos: o ShadowMountPlus monta e põe o Lineup na tela inicial.',
    ],
    update: 'Depois disso, o Lineup procura versões novas quando abre e se atualiza quando você escolhe Atualizar.',
  },
  faq: {
    title: 'Perguntas',
    items: [
      { q: 'Um jogo que eu abri voltou para a frente.', a: 'Essa é a regra do sistema, e o Lineup mantém. Fixe o jogo com □ e salve: dali em diante ele fica no lugar.' },
      { q: 'A ordem continua depois de reiniciar?', a: 'Sim. O Lineup grava onde a tela inicial guarda a fileira, e os apps escondidos continuam escondidos.' },
      { q: 'Funciona no meu firmware?', a: 'Foi testado no 13.60. Se você testar em outra versão, abra uma issue dizendo se funcionou.' },
      { q: 'Como volto para a ordem do sistema?', a: 'Options → Mais opções → Remover fixos de todos os perfis. Todos os perfis voltam para “o último aberto vem primeiro”, e os apps escondidos voltam.' },
      { q: 'Deu algum erro.', a: 'Options → Mais opções → Salvar relatório de bug, e anexe o arquivo numa issue. Ele lista os apps e como a tela inicial guarda cada um, sem nomes nem senhas.' },
    ],
    issue: 'Abrir uma issue',
  },
  footer: {
    star: 'Se o Lineup arrumou sua tela inicial, uma estrela no GitHub ajuda outras pessoas a acharem o projeto.',
    starCta: 'Dar uma estrela no GitHub',
    disclaimer:
      'O Lineup é homebrew para consoles que são seus. Ele só muda a ordem e a visibilidade de apps já instalados e não inclui, baixa nem desbloqueia nenhum jogo, firmware ou chave. É um projeto independente, sem ligação com a Sony Interactive Entertainment nem aprovação dela. “PlayStation” e “PS5” são marcas dos seus donos. Use por sua conta e risco.',
    credits: 'Feito sobre o PS5 React, de half144 e colaboradores.',
    license: 'Licença GPL-3.0',
  },
};

export const strings: Record<Locale, Strings> = { en, 'pt-br': pt };
