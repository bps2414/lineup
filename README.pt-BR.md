# Lineup

Reorganize a fileira de jogos da tela inicial do PS5 pelo controle. Levante um jogo, leve, solte e
salve.

[English](README.md)

![Lineup com Street Fighter 6 em foco na 13ª posição, logo depois do limite da tela inicial](docs/images/home-edge.png)

Lineup é um app homebrew para PS5 desbloqueado. O sistema ordena a fileira pela última vez que você
abriu cada jogo e não deixa você arrumar do seu jeito. O Lineup lê a fileira e deixa você montar a
ordem na TV. Depois, o serviço local dele grava a nova ordem onde a tela inicial guarda a fileira.

## Recursos

- **Levantar, mover, soltar.** ✕ levanta o jogo em foco, o direcional leva e ✕ solta. ◯ durante o
  movimento devolve o jogo ao lugar.
- **Todos os perfis.** Cada perfil do console tem sua fileira, mostrada com a foto dele. Troque pelo
  menu Opções.
- **Limite da tela inicial.** A tela inicial mostra só os 12 primeiros jogos. Um traço na fileira e uma
  marca na régua mostram onde fica esse limite, e os jogos depois dele aparecem como "fora da tela
  inicial".
- **Jogos fixos.** □ fixa o jogo em foco na posição em que ele estiver ao salvar. Abrir outros jogos não
  tira ele do lugar; o resto da fileira segue a regra do sistema. Em Opções dá para fixar a fileira
  toda, desafixar todos ou remover os jogos fixos de todos os perfis. Vale a partir da 1.3.0.
- **Ordenar de A a Z**, descartar mudanças e contagem de jogos que mudaram.
- **Aba Mídia.** L1 e R1 trocam entre as abas Jogos e Mídia. Os apps da Mídia se movem e fixam como os jogos,
  cada aba com os seus fixos.
- **Esconder ou mover apps.** Deixe o foco no app e aperte Opções: Mover e Esconder ficam no topo. Homebrew e
  emuladores mudam entre Jogos e Mídia em todos os perfis. Qualquer app dessas abas, e a PS Store, pode ser escondido
  no perfil atual, continua escondido depois de reiniciar e volta em Opções → Apps escondidos.
- **Aplica ao sair.** Salvar fecha o app, e a tela inicial volta com a nova ordem. Não precisa
  reiniciar.
- **Atualização no app.** O Lineup procura uma versão nova ao abrir e mostra as novidades (Opções →
  Atualizações desliga isso), baixa quando você escolhe **Atualizar** e abre de novo na versão nova. Em Opções →
  Atualizações também dá para voltar à versão anterior. Vale a partir da 1.2.0.
- **Inglês e português do Brasil**, escolhidos pelo idioma do sistema.

![O menu Opções](docs/images/options.png)

## Requisitos

- PS5 com jailbreak, **kstuff** e um **ELF loader na porta 9021** (elfldr, ou o do etaHEN). O Lineup
  envia o serviço para esse loader quando abre.
- **ShadowMountPlus**, para registrar o app na tela inicial.

Testado no firmware **13.60** com kstuff, elfldr 0.26 e ShadowMountPlus. Os outros firmwares não foram
testados.

## Instalação

Baixe os arquivos da [release mais recente](https://github.com/bps2414/lineup/releases/latest).

- **Imagem exFAT (mais fácil).** Copie `Lineup-<versão>.exfat` para `/data/homebrew/` por FTP, ou para
  uma pasta `homebrew/` num pendrive exFAT. O ShadowMountPlus monta a imagem e põe o Lineup na tela
  inicial em alguns segundos.
- **Pasta.** Descompacte `Lineup-<versão>.zip` e envie a pasta `PPSA99077/` para
  `/data/homebrew/PPSA99077` por FTP.

Use um ou outro. Os dois registram o title ID `PPSA99077`.

## Uso

| Botão | Na fileira | Levando um jogo |
| --- | --- | --- |
| ✕ | Levanta o jogo em foco | Solta aqui |
| Direcional | Move o foco | Leva o jogo |
| □ | Fixa ou desafixa o jogo em foco | Fixa ou desafixa o jogo |
| △ | Salva | |
| L1 / R1 | Aba Jogos / aba Mídia | |
| Options | Menu Opções | |
| ◯ | Sai (pergunta se quer salvar as mudanças) | Devolve o jogo |

Abrir um jogo depois continua trazendo ele para a frente, como sempre, a não ser que um jogo fixo ocupe
esse lugar.

## Segurança

- A gravação passa só pelo serviço **lineupd**, numa única transação SQLite. A nova ordem reaproveita os
  índices que a fileira já tem, então nenhum valor passa do que o sistema espera.
- Antes de salvar, o lineupd copia o banco para `/data/lineup/backups/` (guarda os 5 mais recentes) e
  roda o `quick_check` do SQLite. Depois da gravação, roda o `integrity_check`.
- Se a fileira mudou desde a leitura (você abriu um jogo nesse meio-tempo), a gravação é recusada e a
  fileira é carregada de novo.

## Como funciona

O app é escrito em React e roda nativo no console pelo [PS5 React](#ps5-react). Um pequeno serviço em
C, o `lineupd`, vai dentro da pasta do app. O app inicia o serviço pelo ELF loader, e ele responde em
`127.0.0.1:9077`. Ele lê as tabelas de cada perfil em `/system_data/priv/mms/app.db` e grava a nova
ordem. Os jogos fixos ficam no lugar graças a triggers do SQLite que ele cria nesse banco;
`POST /pins/off` e `POST /pins/uninstall` tiram tudo de novo.

Veja [docs/pt-BR/ARQUITETURA.md](docs/pt-BR/ARQUITETURA.md) para os componentes, a API HTTP do serviço
e o que se descobriu sobre como a tela inicial guarda a fileira.

## PS5 React

O [PS5 React](https://github.com/half144/ps5-react), do half144, é um framework para homebrew de PS5
em tela cheia escrito em React e JSX. Ele roda o app com QuickJS e Embedded React, desenha a interface
em software e apresenta com OpenGL, e dá ao app controle, navegação por foco, animação, um subconjunto
do Tailwind, HTTP e acesso a arquivos. Também tem um preview nativo no macOS e gera a pasta do app para
PS5 a partir do mesmo código.

O framework fica em [`ps5-react/`](ps5-react) neste repositório, com um acréscimo que o Lineup precisa:
`Payloads.start(nome)` mais uma etapa no build. A etapa compila os `payloads/*.c` do app em ELFs, e o
`Payloads.start` envia um deles ao ELF loader do console. O resto está como foi publicado.

## Compilar

Linux ou WSL: `scripts/test.sh`, `scripts/build.sh`, `scripts/package.sh <versão>`. Veja
[docs/pt-BR/COMPILAR.md](docs/pt-BR/COMPILAR.md). O CI compila cada push, e tags `vX.Y.Z` publicam uma
release com a imagem exFAT, o zip e o código-fonte completo.

## Créditos

PS5 React — created by half144 and PS5 React contributors.
https://github.com/half144/ps5-react

As dependências do framework mantêm suas licenças e avisos: veja [ps5-react/NOTICE](ps5-react/NOTICE)
e a pasta `notices/` de cada build. A interface do Lineup usa a fonte Inter (SIL Open Font License) e
sons derivados dos recursos de som do Material Design do Google (CC BY 4.0).

## Licença

GPL-3.0-or-later, com o termo de atribuição do PS5 React em
[ps5-react/LICENSE-ATTRIBUTION](ps5-react/LICENSE-ATTRIBUTION). Veja [LICENSE](LICENSE).

O Lineup não tem ligação com a Sony Interactive Entertainment nem é endossado por ela. "PlayStation" e
"PS5" são marcas de seus donos. Use por sua conta e risco.
