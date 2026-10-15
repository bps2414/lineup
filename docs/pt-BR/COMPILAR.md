# Compilar o Lineup

[English](../BUILDING.md)

Linux (o CI usa Ubuntu 24.04) ou WSL. O build baixa dependências fixadas e conferidas por hash (SDK de
payloads do PS5 etc.) em `ps5-react/.deps`.

## Pré-requisitos

```sh
sudo apt-get install -y build-essential cmake git curl unzip zip xz-utils zstd binutils \
  python3-venv llvm-18 llvm-18-dev clang-18 lld-18 libssl-dev
sudo apt-get install -y exfatprogs exfat-fuse   # só para a imagem .exfat
```

Node.js 22 com npm e Python 3.12 ou mais novo. O LLVM é procurado em `/usr/lib/llvm-18/bin`; para usar
outro caminho, defina `LLVM=/caminho/bin`.

## Clonar

```sh
git clone https://github.com/bps2414/lineup.git
cd lineup
```

`ps5-react/` é o framework PS5 React, guardado neste repositório com as mudanças que o Lineup precisa
(veja [ARQUITETURA.md](ARQUITETURA.md)). O `.gitattributes` faz o checkout de todos os arquivos com fim
de linha LF, que os patches do framework exigem.

## Passos

```sh
scripts/test.sh              # autoteste do serviço lineupd no PC -> out/
scripts/build.sh             # npm ci + venv + build completo para PS5 -> ps5-react/dist/PPSA99077/
scripts/package.sh 1.1.0     # -> dist/
```

Um build bom termina com `PASS: title metadata, icon, PS5 ELF/segments, SELF integrity, hashes and ZIP`.

O `package.sh` grava em `dist/`:

- `Lineup-<versão>.exfat`: imagem exFAT para o ShadowMountPlus, com clusters de 64 KB, rótulo `LINEUP`
  e o app na raiz da imagem. A imagem é preenchida por um loop mount, então precisa de `sudo`; use
  `--no-exfat` para pular essa etapa.
- `Lineup-<versão>.zip`: contém `PPSA99077/`, para instalar por FTP em `/data/homebrew/PPSA99077`.
- `Lineup-<versão>-source.tar.gz`: o código do repositório, framework incluído (o código-fonte
  correspondente que a GPL exige).
- `SHA256SUMS`

## Releases

1. Ponha a mesma versão `X.Y.Z` no `app/api.js` (`export const VERSION`) e no
   `app/payloads/lineupd.c` (`#define LINEUPD_VERSION`). No `app/app.json`, ponha `"version"` no
   formato `01.001.000`.
2. Adicione uma seção `## X.Y.Z` no `CHANGELOG.md`; ela vira as notas da release.
3. Faça o commit e rode `git tag vX.Y.Z && git push origin main vX.Y.Z`.

O `.github/workflows/release.yml` falha se a tag não bater com o `api.js` ou o `lineupd.c`. Depois ele
roda o teste, o build e o empacotamento, e cria a release no GitHub com tudo que estiver em `dist/`. O
`ci.yml` faz o mesmo build a cada push e PR, com a versão `-ci.<sha>`, e guarda `dist/` como artefato.
