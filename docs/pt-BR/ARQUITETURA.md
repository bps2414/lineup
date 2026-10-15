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
2. Se outra versão responder, o app pede para ela sair (`POST /quit`). Também manda `POST /sair` com
   `X-Fileira: 1`, que para o serviço de antes da renomeação.
3. Senão, o app chama `Payloads.start('lineupd.elf')` e consulta `/version` por até 5 segundos.
4. Ele lê `/profiles`, escolhe o usuário em primeiro plano (`Users.getForeground()`) e lê o `/row`
   desse usuário.

O `VERSION` do `api.js` e o `LINEUPD_VERSION` do `lineupd.c` precisam ser iguais. O workflow de release
confere os dois com a tag.

## API do serviço

Todas as rotas ficam em `127.0.0.1:9077` e respondem JSON, salvo indicação. Um POST precisa do
cabeçalho `X-Lineup: 1`, para que uma página web não consiga disparar um com um formulário simples.

| Rota | Resposta |
| --- | --- |
| `GET /version` | `{"version"}` |
| `GET /profiles` | `{"profiles":[{"userId","hex","name"}]}` |
| `GET /row?user=<10 dígitos>` | `{"user","games":[{"titleId","name","index","icon"}]}`, na ordem da tela inicial |
| `GET /icon/<titleId>` | `image/png`, o ícone que a tela inicial desenha |
| `POST /row?user=<10 dígitos>` | corpo `LIN1\n` e depois `<titleId> <índice>\n` por jogo, na nova ordem, com o índice lido. `{"ok","backup","integrity"}` |
| `POST /quit` | `{"ok":true}`, e sai |

Os erros vêm como `{"error": "<detalhe em inglês>"}`:

- 400: pedido malformado.
- 403: falta o cabeçalho `X-Lineup`.
- 404: o perfil ou o ícone não existe.
- 409: a fileira mudou desde a leitura.
- 500: falha no banco.

O app mostra um título próprio no idioma da interface e põe o detalhe depois.

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
