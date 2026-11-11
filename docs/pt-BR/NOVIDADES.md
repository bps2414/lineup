# Novidades

As notas que o app mostra ao atualizar. Cada versão aqui acompanha a mesma seção do `CHANGELOG.md`.

## 2.0.1

- Mover um app cujo `AppInfoJson` é uma lista de campos, como firmwares antigos e alguns instaladores de pacote
  gravam, agora muda a aba, a categoria e o link dentro dessa lista. Antes o Lineup criava chaves novas ao lado e
  deixava a lista na aba antiga. Essas chaves somem quando o app muda de novo ou o Lineup abre.
- O README diz quais firmwares foram verificados: um console no 13.60 e cópias dos bancos da tela inicial do 11.40 e
  do 13.00. Um formulário novo recebe relatos de firmware.

## 2.0.0

- Salvar não falha mais com "A fileira mudou" quando há apps fixados. Com o Lineup aberto, o console renumera os
  apps fixados a cada meio minuto; agora o salvar confere a ordem, não os números exatos (#19).
- Todos os apps, o bloco da biblioteca na aba Mídia, pode ser escondido e trazido de volta em Apps escondidos, como
  a PS Store e a Galeria de mídia (#14).
- Ir para a aba Mídia logo depois de abrir o Lineup espera o serviço em vez de falhar depois de alguns segundos, e
  pede de novo uma vez (#15).
- Mensagens em português mais curtas, para caber na notificação (#16).
- O QR code da página da estrela aparece inteiro, e o celular consegue ler. Ele era cortado pela metade (#21).
- A Galeria de mídia muda entre as abas Jogos e Mídia (foque nela na fileira, Opções, Mover) e fica lá depois de
  reiniciar ou do modo de descanso. A PS Store e Todos os apps só escondem. A biblioteca de jogos nunca muda nem
  esconde.
- A Galeria de mídia fica na fileira da aba dela: dá para fixar e mover na fileira como um app.
- As Boas-vindas podem aparecer, em todos os perfis, e ser escondidas de novo (Opções, Apps escondidos).
- Qualquer outro bloco do sistema que aparece numa aba pode ser escondido, e continua escondido.
- A Galeria de mídia volta ao lugar dela no grupo do sistema da fileira de Jogos quando sai de Mídia.
- Mover um app agora move o conceito dele junto, que a 1.8.0 deixava na aba antiga. Apps movidos antes são
  corrigidos quando o Lineup inicia.
- Mais opções tem Remover restos do TabShifter: o bloco "TS" sai dos dois bancos, e as pastas dele também. A
  mensagem avisa quando só sobravam as pastas.
- Backups: na primeira vez que esta versão roda, ela guarda os dois bancos de vez em /data/lineup/backups
  (original-app.db, original-appinfo.db), antes de as próprias verificações gravarem. Depois de atualizar, eles já
  têm o que versões anteriores do Lineup mudaram, então não são uma cópia de antes do Lineup. Dois backups no mesmo
  segundo não se sobrescrevem mais.
- Triggers e gravações só usam as colunas que o firmware tem.
- Ao abrir, o Lineup troca um lineupd rodando de outra versão ou build.

## 1.8.0

- Um app movido continua na aba nova quando é instalado de novo. O payload do Game Compressor instala o atalho de
  novo toda vez que inicia, e isso jogava ele de volta para Jogos (#11).
- O próprio Lineup agora pode ir para a aba Mídia (#12).
- Apps escondidos tem um botão para a Galeria de mídia, igual ao da PS Store (#12).
- O relatório de bug agora diz onde cada app está instalado.
- Mais opções tem Dar uma estrela no GitHub: um QR code para abrir o repositório no celular.

## 1.7.0

- O que você escondeu continua escondido depois de reiniciar o console. Antes a PS Store voltava sozinha, e se ela
  voltou na 1.6.0, abrir o Lineup esconde ela de novo. Um app escondido que for removido e instalado de novo também
  volta escondido.
- Opções não passa mais da tela. Ficou mais curto, as páginas rolam e o último item sempre dá para alcançar.
- Opções agora abre no app em foco: Mover e Esconder ficam logo no topo. Mover só aparece para apps que podem mudar
  de aba.
- Apps escondidos tem página própria, com a opção da PS Store e tudo o que dá para trazer de volta. Ela continua
  aberta depois de cada um.
- Ordenar e fixar toda a fileira foram para Organizar a fileira. Atualizações, o relatório de bug e remover os fixos
  foram para Mais opções.
- Depois de uma mudança, o foco fica onde estava e a mensagem diz o nome do app.
- Trazer um app de volta ou mexer na PS Store descartava mudanças não salvas na ordem. Agora pede para salvar ou
  descartar antes.
- Mover de aba foi testado depois de reiniciar, e o app fica na aba nova.

## 1.6.0

- Opções, Esconder ou mover apps. A lista age sobre o app em foco na fileira, então escolha ele antes.
- Mover para Mídia e Mover para Jogos: homebrew e emuladores mudam de aba em todos os perfis, com tudo o que o
  console guarda sobre a aba mudando junto, não só o número dela. Jogos, títulos de PS4 e apps do sistema não mudam
  de aba, e o app movido perde o lugar fixo. Ainda não foi testado depois de reiniciar o console: se um app voltar
  para a aba antiga, avise.
- Esconder: o app sai da aba e da biblioteca no perfil atual, e volta pela mesma página. A PS Store tem uma opção
  própria ali. Remover apps fixos de todos os perfis também traz de volta o que estava escondido.
- Toda mudança faz antes um backup do banco da tela inicial em `/data/lineup/backups`. Mover guarda também o
  `appinfo.db`.

## 1.5.0

A 1.4.3 não saiu; as mudanças dela vêm aqui.

- L1 e R1 trocam entre as abas Jogos e Mídia. A aba Mídia funciona como a de Jogos: dá para mover e fixar os
  apps (o jailbreak, o Shadow Mount+, um gerenciador de arquivos), e eles ficam na tela inicial em vez de cair na
  biblioteca. Cada aba tem os seus fixos, e um dos 11 primeiros lugares da Mídia fica livre para o último app
  aberto. Mudanças não salvas numa aba precisam ser salvas ou descartadas antes de trocar.
- Abrir, instalar ou remover um app só reordena a aba dele. Os fixos salvos por uma versão anterior passam a
  funcionar assim sozinhos, depois de um backup, na primeira vez que o Lineup abre.
- Em Opções, "Salvar um relatório para um bug" grava `/data/lineup/report.json`: os apps das abas Jogos e Mídia e
  como a tela inicial guarda cada um, com o número de cada perfil mas sem nomes nem senhas. Copie por FTP e anexe
  na sua issue.

## 1.4.2

- Um lugar da tela inicial sempre fica sem fixar. Com todos fixos, o último jogo aberto (a entrada do
  jailbreak, por exemplo) ficava sem lugar e ia parar na biblioteca de jogos. "Fixar toda a fileira" agora deixa
  o primeiro lugar livre, e o Lineup não salva uma tela inicial toda fixa.
- Uma fileira toda fixa por uma versão anterior abre oferecendo liberar o primeiro lugar.

## 1.4.1

- O jogo levantado fica por cima: ao levar ele para a esquerda, o jogo que ele ultrapassa não passa mais por
  cima dele.
- Uma versão nova aparece sozinha ao abrir o Lineup, com as novidades. "Agora não" pula essa versão.
- As novidades cabem na tela e vêm em português.
- A atualização guarda uma cópia da versão instalada antes de fechar o app, então a troca depois é mais
  rápida. Se você abrir o Lineup antes de ela terminar, ele espera e abre a versão nova sozinho.
- A versão nova já encontra o serviço dela rodando, então abre sem precisar trocá-lo.
