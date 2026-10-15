# Plano: atualizador no app, fotos dos perfis e jogos fixos

Status: proposta. Nada aqui foi implementado nem testado no console ainda.

Três funcionalidades, na ordem em que recomendo entregar:

| Versão | Conteúdo | Risco |
| --- | --- | --- |
| 1.2.0 | Fotos dos perfis + atualizador | Baixo / médio |
| 1.3.0 | Jogos fixos (entregue pelo próprio atualizador) | Alto: mexe no esquema do `app.db` |

O atualizador vai primeiro: assim a 1.3.0 e as correções seguintes chegam sem FTP.

Antes das três, uma **fase 0** de diagnóstico no console responde o que a pesquisa não conseguiu
confirmar.

---

## Fase 0: diagnóstico no console

Uma build de teste com uma rota `GET /diag` no `lineupd`. Ela responde em JSON:

1. **Avatares.** A listagem de `/system_data/priv/cache/profile/` (nomes, tamanhos, mtime) e de
   `/user/home/<hex>/`.
2. **Instalação.** O resultado de `FileSystem.mounts()` visto pelo app: onde `/app0` está montado.
   - pasta: `/data/homebrew/PPSA99077` via nullfs?
   - imagem: `/mnt/shadowmnt/Lineup-1.1.0_<hash>`?
   Inclui também a busca por `PPSA99077` e `Lineup-*.exfat` em `/data/homebrew`, `/mnt/usb*/homebrew`,
   `/mnt/ext*/homebrew` e `/data/etaHEN/games`.
3. **SQLite do sistema.** As strings `3.x.y` e `SQLITE_OMIT_*` encontradas em
   `/system/common/lib/libSceSqlite.sprx` (ou no módulo que o SceShellUI carrega). Isso diz se o
   SQLite do sistema aceita triggers.
4. **Como o sistema grava o índice.** Uma rota `POST /diag/trace?user=…` instala, depois de um
   backup, um trigger **só de log** em `tbl_iconinfo_<user>`. O trigger grava `UPDATE`, `INSERT` e
   `DELETE` em `lineup_trace(op, titleId, old, new, at)`. Depois o usuário abre e fecha um jogo, e
   `GET /diag/trace` mostra o que aconteceu. `DELETE /diag/trace` remove o trigger e a tabela.

O item 4 é o mais importante para os jogos fixos: ele mostra se o sistema usa `UPDATE` ou
`INSERT OR REPLACE`, quantas gravações faz ao abrir e ao fechar, e se o shell continua lendo o banco
com um trigger desconhecido. O teste é feito com backup e um plano de volta por FTP já pronto.

---

## 1. Fotos dos perfis

### O que a pesquisa encontrou

- O PS5 guarda a foto de cada conta em `/system_data/priv/cache/profile/0x<id>/`.
- Os arquivos são `avatar.png` (o original, 440×440) e versões `.dds` de 440, 260, 128 e 64 px.
  Ferramentas de troca de avatar (Hotswap, Vox Avatar Payload, ProsperoMgr) escrevem nessa pasta.
- O nome da pasta é o id da conta em hex com `0x`. Os exemplos variam entre 8 e 16 dígitos, então
  não se deve presumir o formato.
- O `<Image>` do PS5 React decodifica só PNG e JPEG, então o `avatar.png` serve e os `.dds` não.
- O `lineupd` já lê `/system_data/priv`, porque o `app.db` está lá.

### Serviço (`lineupd.c`)

- `GET /profiles` ganha `"avatar": <mtime ou 0>` em cada perfil, para invalidar o cache de imagem,
  como o `icon` dos jogos.
- Nova rota `GET /avatar/<userId>` (10 dígitos):
  1. Abre `/system_data/priv/cache/profile/` e procura a entrada cujo nome, sem `0x` e lido como hex
     com `strtoull`, vale o `userId`. Compara o valor inteiro e também os 32 bits baixos. Assim
     funciona com 8 ou 16 dígitos.
  2. Serve `avatar.png` dessa pasta como `image/png`, reaproveitando o código de `get_icon`.
  3. Responde 404 quando não acha. O app mostra a inicial do nome.
- Valida o `userId` com `is_user_id` e rejeita `..`, como nas outras rotas.
- Teste no `LINEUPD_TEST`: uma pasta de perfil falsa em `/tmp` com 8 e com 16 dígitos.

### App

- `api.avatarUrl(profile)` → `${BASE}/avatar/${userId}?m=${avatar}`.
- Componente `Avatar({profile, size})` em `ui.jsx`:
  - um círculo com a inicial do nome sobre uma cor derivada do nome (hash → paleta fixa);
  - o `<Image>` por cima, com `rounded-full`; se a imagem falhar, a inicial continua visível.
- `SheetItem` ganha a prop opcional `leading` (um nó à esquerda do rótulo).
- Onde aparece:
  - **Opções → Trocar perfil**: a foto (40 px) ao lado de cada nome. É o pedido principal.
  - **Opções**: a foto do perfil atual no subtítulo do menu.
  - **Cabeçalho**: a foto (36 px) antes do nome do perfil.
- `Image.prefetch` dos avatares logo depois de `/profiles`, para a lista abrir sem piscar.

### Esforço

Pequeno. Cerca de 60 linhas de C e 60 de JSX. Para fechar, falta só o item 1 da fase 0.

---

## 2. Atualizador no app

### Peças que já existem

| Peça | Onde | Para quê |
| --- | --- | --- |
| `Http.request` | HTTPS com verificação TLS pela lista de CAs do console, até 5 redirects | Ler o manifesto |
| `Downloads.enqueue` | Binário nativo, retomável, com `sha256` | Baixar o zip ou o `.exfat` |
| `Archives.extract` | zip com libarchive, validação de caminhos, publicação atômica | Abrir o zip |
| `sceLncUtilGetAppId` / `sceLncUtilLaunchApp` | `relauncher.c` | Esperar o app fechar e abri-lo de novo |
| `lineupd` | Já roda fora do app e sobrevive ao fechamento | Trocar os arquivos com o app fechado |
| `release.yml` | Já publica zip, `.exfat` e `SHA256SUMS` | — |

### O que a pesquisa diz sobre o ShadowMountPlus

- Ele procura pastas (`/data/homebrew/<TITLE_ID>/sce_sys/param.json`) e imagens `.exfat` a cada 15 s
  (`scan_interval_seconds`).
- Ele só processa uma fonte nova depois de 10 s estável (`stability_wait_seconds`).
- Imagens são montadas **somente leitura** por padrão (`mount_read_only=1`), quando o app abre, e
  liberadas quando ele fecha. A exceção é `persistent_image_mounts=1`.
- Duas fontes com o mesmo title ID geram aviso de duplicata. Só pode haver **uma** por vez.

### Fluxo

1. **Verificar.** Ao abrir, em segundo plano e no máximo uma vez a cada 24 h; também pela opção
   **Opções → Procurar atualização**. O app lê
   `https://github.com/bps2414/lineup/releases/latest/download/update.json`.
   - Esse link redireciona para o arquivo da última release, sem usar a API do GitHub. Assim não há
     limite de 60 requisições por hora nem exigência de `User-Agent`.
   - Sem cabeçalhos customizados, o `Http` segue os redirects.
2. **Avisar.** Um toast diz "Lineup 1.2.0 disponível" e um item aparece no menu. A tela de
   atualização mostra as notas da versão (vindas do `update.json`), o tamanho e os botões
   **Atualizar** e **Agora não**. Nada é baixado sem confirmação.
3. **Baixar.** `Downloads.enqueue` grava em `/data/lineup/update/<versão>/`, com o `sha256` do
   manifesto e `expectedBytes`, mostrando o progresso.
   - Instalação em pasta: baixa o zip.
   - Instalação em imagem: baixa o `.exfat`.
4. **Preparar.**
   - Zip: `Archives.extract` para `/data/lineup/update/<versão>/stage/`.
   - Depois, verifica `PPSA99077/sce_sys/param.json` (title ID e versão) e a presença de
     `eboot.bin` e `lineupd.elf`.
5. **Aplicar.** O app chama `POST /update/apply` no `lineupd` com `{versão, tipo, destino}`.
   - O `lineupd` responde "pronto" e o app chama `BackHandler.exitApp()`.
   - Em seguida o `lineupd` faz o mesmo que o `relauncher.c`: espera `sceLncUtilGetAppId("PPSA99077")`
     ficar negativo (no máximo 30 s), depois mais 500 ms.
   - **Pasta.** Copia a pasta atual para `/data/lineup/update/previous/`. Depois, para cada arquivo
     novo, grava `.<nome>.new`, faz `fsync` e `rename` por cima. Apaga os arquivos que não existem
     mais. A troca é arquivo por arquivo, dentro da mesma pasta, então continua visível pela
     montagem nullfs.
   - **Imagem.** Move o `.exfat` antigo para `/data/lineup/update/previous/`, que fica fora da busca
     do ShadowMountPlus. Depois move o novo para o mesmo diretório, com o nome
     `Lineup-<nova>.exfat`. Assim só existe uma fonte com `PPSA99077`. Em seguida espera o
     ShadowMountPlus aceitar a fonte nova: cerca de 25 s, o intervalo de busca mais o tempo de
     estabilidade.
   - Abre o app de novo com `sceLncUtilLaunchApp` para o usuário em primeiro plano e termina.
   - O app novo encontra um `lineupd` de versão diferente e o substitui. Essa lógica já existe em
     `ensureService`.
6. **Confirmar.** O app novo grava `/data/lineup/update/ok-<versão>` e mostra "Atualizado para
   1.2.0". A opção **Voltar à versão anterior** usa `previous/`, pelo mesmo caminho do passo 5.

### `update.json`, gerado pelo `scripts/package.sh`

```json
{
  "version": "1.2.0",
  "notes": {"en": "…", "pt-BR": "…"},
  "zip":   {"name": "Lineup-1.2.0.zip",   "size": 0, "sha256": "…"},
  "exfat": {"name": "Lineup-1.2.0.exfat", "size": 0, "sha256": "…"}
}
```

- `release.yml` passa a publicar esse arquivo junto com os outros.
- As notas vêm do `CHANGELOG.md`, que o workflow já lê.

### Segurança

- O HTTPS é verificado e o SHA-256 vem do manifesto. O manifesto, porém, vem do mesmo lugar que os
  arquivos: se alguém controlar a conta do GitHub, controla os dois.
- **Recomendado (fase 2):** assinar o `update.json` com Ed25519.
  - A chave privada fica num secret do Actions.
  - A pública fica embutida no `lineupd`.
  - A verificação usa o Monocypher (domínio público / BSD-2, um único `.c`), compilado junto com o
    payload.
- Não aceitar downgrade, exceto pelo **Voltar à versão anterior**.
- Ignorar `version` menor ou igual à atual.
- Todas as rotas novas exigem `X-Lineup: 1`. Os caminhos de destino são validados no `lineupd`, nunca
  confiados ao corpo da requisição.

### Configuração

- **Opções → Atualizações**, com:
  - "Procurar ao abrir" (ligado por padrão);
  - "Procurar agora";
  - "Voltar à versão anterior", quando houver uma.
- Grava em `/data/lineup/settings.json` com `FileSystem.writeFile` (o app tem acesso ao console).

### Riscos para testar no console

- Instalação em imagem com `persistent_image_mounts=1`: a imagem continua montada. Nesse caso o
  `lineupd` detecta a montagem e não troca o arquivo. O app avisa que o arquivo foi baixado e que
  precisa ser trocado à mão.
- Se a pasta for registrada por cópia, não por montagem, o ShadowMountPlus pode não ver a mudança.
  O item 2 da fase 0 responde isso.
- `Archives.extract` e `Downloads` ainda não têm evidência de hardware no `HARDWARE.md` do
  framework. O primeiro teste da 1.2.0 cobre isso.

### Esforço

Médio:

- cerca de 200 linhas de C (`/update/apply`, cópia e troca, espera e relançamento);
- cerca de 250 de JS/JSX (verificação, tela, progresso, configuração);
- mudanças no `package.sh` e no `release.yml`.

---

## 3. Jogos fixos

### O problema

- A ordem é `lastAccessIndex`, do maior para o menor.
- Ao abrir ou fechar um jogo, o sistema dá a ele um índice novo, maior que todos. Por isso o jogo vai
  para a frente e desmancha a ordem salva.
- O shell relê a tabela quando um app fecha.

### Escolha: trigger dentro do `app.db` (plano A)

Um trigger do SQLite roda **dentro da própria gravação do sistema**, na mesma transação. Assim que o
sistema grava o índice novo, o trigger refaz a ordem fixa. Quando o shell relê a tabela, já encontra
a ordem certa.

Vantagens:

- não precisa de processo residente;
- sobrevive a reinícios, porque fica gravado no banco;
- não tem corrida com a releitura do shell.

Plano B, se a fase 0 mostrar que triggers não servem:

- um vigia residente (`lineup-pind.elf`) lê `PRAGMA data_version` a cada 2 s e reaplica a ordem;
- desvantagens: precisa ser iniciado depois de cada boot (pelo autoload do etaHEN ou ao abrir o
  Lineup), e o shell pode mostrar a ordem errada até a próxima releitura.

### Semântica

- **Fixar** prende um jogo na **posição** em que ele está quando o usuário salva (por exemplo, "3º").
- Os demais jogos continuam com o comportamento do sistema: o último aberto vai para a primeira
  posição livre, e os outros ocupam as posições livres na ordem de uso.
- **Fixar toda a fileira** é o mesmo mecanismo com todos os jogos fixos.
  - Abrir um jogo não muda nada.
  - Um jogo instalado depois entra na primeira posição livre. Se não houver, vai para o fim.
- Se a fileira encolher (um jogo for desinstalado), um jogo fixado além do fim vai para o último
  lugar. Um jogo fixo que for desinstalado é ignorado.

### Banco

Dentro do `app.db`, porque um trigger só enxerga o próprio banco:

```sql
CREATE TABLE IF NOT EXISTS lineup_pin (user TEXT, titleId TEXT, slot INTEGER, PRIMARY KEY (user, titleId));
CREATE TABLE IF NOT EXISTS lineup_seq (i INTEGER PRIMARY KEY);      -- 0..511, para "k-ésima posição livre"
CREATE TABLE IF NOT EXISTS lineup_tmp (titleId TEXT PRIMARY KEY, v INTEGER, r INTEGER, nv INTEGER);
CREATE TRIGGER lineup_pin_u_<user> AFTER UPDATE OF lastAccessIndex ON tbl_iconinfo_<user>
  WHEN NEW.lastAccessIndex > OLD.lastAccessIndex BEGIN <reordenar>; END;
CREATE TRIGGER lineup_pin_i_<user> AFTER INSERT ON tbl_iconinfo_<user> BEGIN <reordenar>; END;
CREATE TRIGGER lineup_pin_d_<user> AFTER DELETE ON tbl_iconinfo_<user> BEGIN <reordenar>; END;
```

`<reordenar>` é escrito só com SQL antigo: nada de funções de janela, CTE, UPSERT ou `RETURNING`.
Não sabemos a versão do SQLite do shell, e CTEs nem são aceitas dentro de triggers.

1. `DELETE FROM lineup_tmp`. Depois, copiar `(titleId, lastAccessIndex)` da fileira visível
   (`visible = 1 AND dispLocation = 138`).
2. `r` de um jogo fixo é `min(slot, n-1)`.
3. `r` de um jogo não fixo: seja `k` o número de não fixos com índice maior. `r` é a k-ésima posição
   livre: `SELECT MIN(i) FROM lineup_seq WHERE i < n AND i NOT IN (slots) AND i - (SELECT COUNT(*) FROM slots WHERE slot < i) = k`.
4. `nv` = o r-ésimo maior dos índices que a fileira **já tem** (contagem correlata). É a mesma regra
   do `save_order`: nenhum índice passa do contador que o shell guarda na memória. O maior possível é
   o que o próprio sistema acabou de dar.
5. `UPDATE tbl_iconinfo_<user> SET lastAccessIndex = (SELECT nv …) WHERE titleId IN (SELECT titleId FROM lineup_tmp WHERE nv <> v)`.
   - `recursive_triggers` vem desligado por padrão, então esse `UPDATE` não dispara o trigger de
     novo.
   - A condição `NEW > OLD` também impede isso, porque a reordenação só reaproveita índices.

Custo: O(n²) com n ≈ 30–100 jogos, alguns milissegundos dentro da gravação do sistema.

### Mudanças no `lineupd`

- `save_order`:
  - dentro do `BEGIN IMMEDIATE`, faz `DROP TRIGGER` dos triggers do usuário;
  - aplica a ordem;
  - grava `lineup_pin`;
  - recria os triggers, se houver jogos fixos;
  - faz `COMMIT`.

  Sem isso, os `UPDATE` do próprio Lineup disparariam os triggers. O backup e as checagens continuam
  como hoje.
- `GET /row` devolve `"pinned": slot | null` em cada jogo e `"pins": "on" | "missing"`. `missing`
  indica que a tabela tem fixos mas os triggers sumiram, por exemplo depois de "Reconstruir banco de
  dados" ou de uma atualização do sistema. O app avisa e oferece reaplicar.
- `POST /pins/off?user=…` remove os triggers e os fixos daquele perfil.
- `POST /pins/uninstall` remove tudo o que o Lineup criou no `app.db`. Fica no menu como "Remover
  jogos fixos de todos os perfis" e é o caminho de volta.
- Teste no `LINEUPD_TEST`, no banco sintético que já existe:
  - simular "abrir jogo" (`UPDATE … SET lastAccessIndex = max+1`), instalar (`INSERT`) e desinstalar
    (`DELETE`), e conferir que os fixos ficam no lugar;
  - rodar contra SQLite 3.8 e 3.46, e também com uma build `SQLITE_OMIT_TRIGGER`, para ver a falha.

### App

- **□ (quadrado)** fixa ou desafixa o jogo em foco. A dica nova é "Fixar". A mudança só vale ao
  salvar, como o resto.
- Um ícone de alfinete no canto do jogo fixo e um tique diferente na régua.
- O texto da posição diz "fixo no 3º".
- Ao mover um jogo fixo, ele fica fixo na posição nova. Os não fixos se acomodam em volta.
- Opções:
  - "Fixar toda a fileira";
  - "Desafixar todos";
  - "Remover jogos fixos de todos os perfis".
- O contador de mudanças passa a incluir as mudanças de fixação.

### Riscos (por que fica na 1.3.0)

- **O maior:** se o SQLite do shell for compilado sem triggers, ou não entender a sintaxe, o esquema
  deixa de abrir para o sistema e a tela inicial quebra. A fase 0 (itens 3 e 4) existe para provar
  isso antes, com backup e restauração por FTP já testada.
- "Reconstruir banco de dados" e algumas atualizações do sistema podem recriar o `app.db`. Nesse caso
  os fixos somem; o app detecta (`pins: "missing"`) e oferece reaplicar.
- Se o sistema gravar com `INSERT OR REPLACE`, o caminho é o trigger de `INSERT`, já previsto. O
  item 4 da fase 0 confirma qual dos dois acontece.

### Esforço

Médio a grande:

- cerca de 250 linhas de C/SQL e cerca de 150 de JSX;
- o tempo maior vai para testar no console.

---

## Ordem de trabalho

1. Fase 0: uma build de diagnóstico, rodada uma vez no console. O usuário envia o JSON do `/diag`.
2. 1.2.0: fotos dos perfis, `update.json` no `package.sh`/`release.yml` e o atualizador.
   - A **primeira** atualização automática só acontece da 1.2.0 para a 1.2.1, porque a 1.1.0 não tem
     atualizador.
   - Publicar uma 1.2.1 trivial logo depois serve para testar o caminho de ponta a ponta.
3. 1.3.0: jogos fixos, entregue pelo atualizador.
4. Documentação em cada etapa: `README` (en e pt-BR), `ARCHITECTURE.md`/`ARQUITETURA.md` com as rotas
   novas e o `CHANGELOG.md`.

## Decisões em aberto

- Fixar por **posição** (proposto) ou só um bloco fixo no início da fileira (mais simples)?
- Procurar atualização ao abrir: ligado por padrão (proposto) ou só manual?
- Assinatura Ed25519 do manifesto já na 1.2.0 ou depois?

## Fontes

- [ShadowMountPlus (README)](https://github.com/drakmor/ShadowMountPlus): busca, montagem somente
  leitura, `persistent_image_mounts`, duplicatas.
- [Vox Avatar Payload](https://github.com/VoxDon3/-Vox-Avatar-Payload): instala avatares em
  `/system_data/priv/cache/profile/0x…`.
- [Hotswap, avatar remoto no PS5 (psxhax)](https://www.psxhax.com/threads/hotswap-remote-user-avatar-tool-for-ps5-by-koolaidxk1d-ps5usermod-by-mour0ne.16258/):
  `avatar.png` 440 e `.dds` 440/260/128/64.
- [PS4 Avatar Maker](https://github.com/m2k7m/PS4-AVATAR-MAKER): tamanhos e formato DXT5.
- [PS5-Suite releases](https://github.com/manos555555/PS5-Suite/releases): outra ferramenta que
  ajusta `lastAccessIndex` (MAX+1).
- [Guia de exibição de apps no PS5 (logic-sunrise)](https://www.logic-sunrise.com/forums/topic/104586-tuto-guide-de-modification-de-laffichage-des-applications-sur-ps5-jailbreakee/):
  edições no `tbl_iconinfo` são desfeitas por reconstrução ou refresh.
- No repositório: `ps5-react/docs/NATIVE-API.md` (Archives, BackHandler/relaunch, Payloads),
  `ps5-react/docs/NETWORKING.md` (HTTPS, Downloads) e `ps5-react/native/ps5/payloads/relauncher.c`.
