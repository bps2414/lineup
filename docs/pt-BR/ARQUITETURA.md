# Arquitetura

[English](../ARCHITECTURE.md)

```
app/                 o app Lineup: React/JSX, assets, manifesto (titleId PPSA99077)
app/payloads/        lineupd.c, o serviço local, compilado em lineupd.elf
ps5-react/           o framework PS5 React (half144), com o Payloads.start acrescentado
scripts/             teste, build e empacotamento
.github/workflows/   CI e releases por tag
```

## Componentes

```
┌──────────── PS5 ─────────────────────────────────────────────────────┐
│  App Lineup (eboot.bin)                    lineupd.elf               │
│  React no QuickJS, PS5 React     HTTP      C, SQLite 3.46.1          │
│  desenha a fileira, lê o controle ──────▶  127.0.0.1:9077            │
│        │                                       │                     │
│        │ Payloads.start('lineupd.elf')         │ lê / grava          │
│        ▼                                       ▼                     │
│  ELF loader :9021 ── roda ──▶ lineupd   /system_data/priv/mms/app.db │
└──────────────────────────────────────────────────────────────────────┘
```

**O app** (`app/index.jsx`, `ui.jsx`, `api.js`, `i18n.js`) roda no PS5 React. Cada posição da fileira
tem um slot focável invisível, então a navegação por foco do framework conduz o movimento. Os jogos são
desenhados à parte e animam até os slots. O `i18n.js` escolhe inglês ou português uma vez, pelo
`DeviceInfo.get().language`.

O app precisa de `"networking": true` e `"filesystemAccess": "console"` no `app.json`. Com isso, o PS5
React inicia um helper próprio pelo ELF loader na abertura, e o app consegue falar com o serviço por
HTTP e carregar os ícones que ele serve.

**O serviço** (`app/payloads/lineupd.c`) é um servidor HTTP de uma thread só. Ele atende um pedido por
vez, escuta só no loopback e sai com `POST /quit`. Uma fileira tem poucas dezenas de jogos, então ele
não usa cache.

**O acréscimo no framework** (`ps5-react`, ABI nativa v17):

- O `tools/build_ps5.py` compila cada `app/payloads/<nome>.c` com o `prospero-clang` do SDK, liga o
  SQLite fixado e põe `<nome>.elf` na raiz da pasta do app.
- O `Payloads.start(nome)` envia esse ELF ao loader em `127.0.0.1:9021` e volta sem esperar ele rodar.

A referência está em [ps5-react/docs/NATIVE-API.md](../../ps5-react/docs/NATIVE-API.md#payloads-abi-v17).

## Abertura

1. O app chama `GET /version`. Se receber a mesma versão do `VERSION` do `api.js`, segue.
0. Se `/data/lineup/update/applying` existir, o lineupd ainda está trocando os arquivos: o app mostra
   "Terminando a atualização", espera a marca sumir (no máximo 90 s) e abre de novo com
   `BackHandler.exitApp({relaunch: true})`, porque ele é a versão antiga.
2. Se outra versão responder, o app pede para ela sair (`POST /quit`).
3. Senão, o app chama `Payloads.start('lineupd.elf')` e consulta `/version` por até 5 segundos.
4. Ele lê `/profiles`, escolhe o usuário em primeiro plano (`Users.getForeground()`) e lê o `/row`
   desse usuário.

O `VERSION` do `api.js` e o `LINEUPD_VERSION` do `lineupd.c` precisam ser iguais. O workflow de release
confere os dois com a tag.

## API do serviço

Todas as rotas ficam em `127.0.0.1:9077` e respondem JSON, salvo indicação. Um POST, e toda rota
`/update/`, precisa do cabeçalho `X-Lineup: 1`, para que uma página web não consiga disparar um com um
formulário simples.

| Rota | Resposta |
| --- | --- |
| `GET /version` | `{"version"}` |
| `GET /profiles` | `{"profiles":[{"userId","hex","name","avatar"}]}`; `avatar` é o mtime da foto, ou 0 |
| `GET /row?user=<10 dígitos>[&tab=games\|media]` | `{"user","tab","pins","games":[{"titleId","name","index","icon","pinned"}]}`, na ordem da tela inicial; a aba Mídia (`dispLocation = 188`) não tem fixos; `pinned` é a posição fixa ou `null`, `pins` é `"on"`, `"missing"` (fixos sem os triggers) ou `"off"` |
| `GET /icon/<titleId>` | `image/png`, o ícone que a tela inicial desenha |
| `GET /avatar/<10 dígitos>` | `image/png`, o `avatar.png` de `/system_data/priv/cache/profile/0x<id>/` (8 ou 16 dígitos hex) |
| `POST /row?user=<10 dígitos>` | corpo `LIN1\n` e depois `<titleId> <índice>[ pin]\n` por jogo, na nova ordem, com o índice lido; ` pin` fixa o jogo naquela posição. `{"ok","backup","integrity"}` |
| `POST /pins/off?user=<10 dígitos>` | `{"ok","backup"}`: desafixa os jogos daquele perfil e apaga os triggers dele |
| `POST /pins/uninstall` | `{"ok","backup"}`: remove todos os fixos, triggers e tabelas que o Lineup pôs no `app.db` |
| `POST /report` | `{"ok","path"}`: grava `/data/lineup/report.json` com os triggers do `app.db` e, por perfil, os apps das abas Jogos (138) e Mídia (188), mais a `tbl_contentinfo`, só as colunas que diferenciam as duas abas |
| `POST /quit` | `{"ok":true}`, e sai |
| `GET /update/install` | `{"kind":"folder"\|"image","path","previous"}`: a única pasta `PPSA99077` ou imagem `Lineup-*.exfat`, e a versão guardada em `/data/lineup/update/previous/` |
| `POST /update/apply?version=<x.y.z\|previous>` | `{"ok":true}`; depois espera o app fechar, troca os arquivos e abre o app de novo (ver Atualização) |

Os erros vêm como `{"error": "<detalhe em inglês>"}`:

- 400: pedido malformado, ou todos os lugares da tela inicial fixos (veja Jogos fixos).
- 403: falta o cabeçalho `X-Lineup`.
- 404: o perfil ou o ícone não existe.
- 409: a fileira mudou desde a leitura.
- 500: falha no banco.

O app mostra um título próprio no idioma da interface e põe o detalhe depois.

## Jogos fixos

O sistema leva um jogo para a frente toda vez que ele abre. Para um jogo fixo continuar no lugar, o lineupd
cria três triggers na `tbl_iconinfo_<user>` do perfil, dentro do `app.db`: `lineup_pin_u_<user>` (depois de um
update que aumenta o `lastAccessIndex`, o que abrir um jogo faz duas vezes), `lineup_pin_i_<user>` (instalar) e
`lineup_pin_d_<user>` (desinstalar), só para jogos da fileira (`visible = 1` e `dispLocation = 138`): um app da
aba Mídia abre, instala e sai como se o Lineup não existisse. Eles rodam dentro da própria gravação do sistema e reordenam a fileira antes
de a tela inicial ler de novo: os fixos ficam na sua posição (vão para o fim se a fileira encolher) e os outros
ocupam as posições livres pela ordem de uso. Como ao salvar, a reordenação só reaproveita índices que a fileira
já tem. As posições ficam em `lineup_pin`; `lineup_seq` e `lineup_tmp` são tabelas auxiliares. O SQL não usa
CTE, funções de janela, UPSERT nem `RETURNING`.

Ao salvar, o lineupd apaga os triggers do perfil, grava a ordem e os fixos e recria os triggers se houver fixos,
tudo numa transação só. Quando nenhum perfil tem mais jogos fixos, as tabelas também saem. Reconstruir o banco de
dados pode apagar os triggers; aí o `/row` responde `"missing"` e o app oferece fixar de novo. Os triggers da
1.4.2 e anteriores também rodavam para a aba Mídia; ao iniciar, o lineupd troca esses pelos atuais, depois de um
backup.

Um dos primeiros `HOME_LIMIT` (12) lugares sempre fica sem fixar: com todos fixos, o último jogo aberto, como a
entrada do jailbreak, fica sem lugar na tela inicial e vai parar na biblioteca de jogos. O app mantém esse lugar
livre ("Fixar toda a fileira" deixa o primeiro) e oferece liberar o primeiro lugar de uma fileira toda fixa pela
1.4.1 ou anterior; o `save_order` recusa essa fileira com 400.

## Atualização

O app lê `https://github.com/bps2414/lineup/releases/latest/download/update.json` (gerado pelo
`scripts/package.sh`: versão, notas, e nome, tamanho e SHA-256 do zip e do `.exfat`) ao abrir e por
Opções → Atualizações. Uma versão nova abre o diálogo na hora, a não ser que o usuário esteja no meio de algo
ou tenha pulado essa versão com "Agora não" (`skipped` em `/data/lineup/settings.json`, junto com
`checkOnOpen`). As notas vêm do `CHANGELOG.md` e do `docs/pt-BR/NOVIDADES.md`, com as linhas quebradas juntadas.

1. `Downloads.enqueue` baixa o zip (instalação em pasta) ou o `.exfat` (em imagem) para
   `/data/lineup/update/<versão>/`, conferido com o tamanho e o SHA-256 do manifesto.
2. O zip é aberto com `Archives.extract` em `stage/`, e o app confere `PPSA99077/sce_sys/param.json`
   (title ID, versão), `eboot.bin` e `lineupd.elf`.
3. `POST /update/apply`. O lineupd acha sozinho a instalação e a origem, recusa versão que não seja mais
   nova, copia a instalação em pasta para `/data/lineup/update/previous.new/` enquanto o app ainda mostra
   "Guardando uma cópia", grava a marca `applying`, responde e espera `sceLncUtilGetAppId` ficar negativo
   (no máximo 30 s), mais 500 ms.
   - Pasta: grava cada arquivo novo como `.<nome>.new` e faz `rename` por cima do antigo, apaga o que a
     versão nova não tem, e o `previous.new/` vira `previous/`. Se falhar, devolve a cópia salva.
   - Imagem: recusa se ela continuar montada (`persistent_image_mounts`), move a antiga para
     `previous/` e a nova para a pasta dela, e espera 25 s pelo ShadowMountPlus.
   - Depois, um `sync()`, a marca sai e o lineupd passa o bastão: fecha a porta, manda o `lineupd.elf`
     instalado para o ELF loader na 9021 e sai (se isso falhar, ele continua atendendo). Então
     `sceLncUtilLaunchApp` para o usuário em primeiro plano, a não ser que o app já esteja aberto de novo.
     Uma falha fica em `/data/lineup/update/error`, e o app mostra.
4. O app novo já encontra o lineupd dele rodando, grava `/data/lineup/update/ok-<versão>` e avisa que foi
   atualizado. Voltar à versão anterior usa `previous/` pelo mesmo caminho.

## Como a tela inicial guarda a fileira

Tudo isto foi descoberto lendo e editando uma cópia do banco e conferindo no console (firmware 13.60):

- **Onde fica a fileira.** Ela está em `/system_data/priv/mms/app.db` (SQLite), uma tabela por perfil:
  `tbl_iconinfo_<userId>`.
- **O ID do perfil.** O `userId` é o nome da pasta do perfil em `/user/home/`, lido em hex e escrito em
  decimal com 10 dígitos: `12f0d5ab` vira `0317773227`. O nome do perfil está em
  `/user/home/<hex>/username.dat`.
- **Quais linhas entram na fileira.** Entram as linhas com `visible = 1` e `dispLocation = 138`. Outros
  locais, como `188`, ficam fora.
- **A ordem.** É o `lastAccessIndex`, do maior para o menor. O `lastAccessTime` não influencia.
- **Abrir ou fechar um jogo.** O sistema dá a esse título um índice novo e maior. Por isso abrir um
  jogo põe ele na frente.
- **Por que a gravação reaproveita índices.** A tela inicial guarda um contador de índices na memória.
  Por isso a gravação dá à ordem enviada os índices que a fileira já tem, do maior para o menor, e
  nunca inventa um maior.
- **Quando a mudança aparece.** A tela inicial relê a tabela quando um app fecha, o próprio Lineup
  inclusive. Também relê depois de abrir e fechar as Configurações. Por isso salvar fecha o Lineup
  sozinho.
- **Ícones.** Eles vêm de `tbl_contentinfo.icon0Info` (um caminho `/user/...`). Se não houver, vêm de
  `/user/appmeta/<titleId>/icon0.png`.
- **O limite de 12 jogos.** A tela inicial mostra os 12 primeiros jogos da fileira. O resto fica só na
  Biblioteca de jogos. O limite foi visto em um console e está em `HOME_LIMIT` no `app/index.jsx`.

## Gravação

`save_order` no `lineupd.c`:

1. `PRAGMA quick_check`. Se falhar, nada é gravado.
2. `VACUUM INTO /data/lineup/backups/app-<hora>.db`, guardando os 5 backups mais recentes.
3. `BEGIN IMMEDIATE` e nova leitura da fileira. A lista enviada precisa ser exatamente os jogos atuais
   com os índices que o app leu. Senão, o serviço responde 409 e desfaz.
4. Atualiza o `lastAccessIndex` de cada jogo, confere que mudou exatamente uma linha por jogo e faz o
   `COMMIT`.
5. `PRAGMA integrity_check`. O resultado é devolvido e registrado no log.

## Testes

O `scripts/test.sh` compila o `lineupd.c` para o PC com `-DLINEUPD_TEST` e roda a autoverificação:

- a lógica de reordenação, e o parser contra entradas malformadas;
- sem argumentos, uma ida e volta num `app.db` sintético: reordena, recusa uma gravação desatualizada,
  restaura e compara;
- com `<cópia do app.db> <userId>`, a mesma ida e volta numa cópia real.
