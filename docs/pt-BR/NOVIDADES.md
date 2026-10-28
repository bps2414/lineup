# Novidades

As notas que o app mostra ao atualizar. Cada versão aqui acompanha a mesma seção do `CHANGELOG.md`.

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
