# Memory card

A API `CARD*` do SDK é atendida em `runtime/hle_card.c`. O protocolo EXI do cartão não é emulado.

- Slot A: cartão de 59 blocos (4 Mbit, setores de 8 KiB). Slot B: vazio.
- Cada arquivo do jogo vira um `.gci` em `saves/slotA`. É o formato GCI: uma entrada de diretório de 0x40 bytes seguida dos blocos de dados, o mesmo que o Dolphin importa e exporta.
- O port segue as regras da pasta GCI do Dolphin, para a mesma pasta servir aos dois:
  - um save é regravado no mesmo arquivo de onde foi lido, qualquer que seja o nome;
  - save novo recebe o nome que o Dolphin daria: `<empresa>-<jogo>-<nome>.gci`, como `7D-GM5E-EA94C7DA_Profile1.gci`. Saves gravados antes dessa mudança têm o nome antigo (`GM5E-7D-...`) e continuam valendo;
  - arquivos cujo tamanho não bate com o número de blocos, ou que repetem um save já carregado (mesmo jogo e nome interno), são ignorados com um aviso no log.
- As versões síncronas que o jogo usa não tinham nome e foram identificadas em `config/GM5E7D/manual_symbols.tsv`: `CARDMount`, `CARDCheck`, `CARDCreate`, `CARDRead`, `CARDWrite`, `CARDFastOpen`, `CARDFormat` e `CARDGetSectorSize`. As assíncronas chamam o callback como uma interrupção.
- `CARDGetStatus` recalcula os offsets de banner e ícone da mesma forma que o SDK.

## Comportamento observado

1. Com o cartão presente, Campaign abre "Profile Settings" (nome "Profile1").
2. Ao confirmar, o jogo cria `EA94C7DA_Profile1` com 4 blocos, banner e ícone C8, comentário "Metal Arms / Profile1" e grava cerca de 8 KiB de dados.
3. Numa nova execução, o jogo lê o arquivo (`0x200` + `0x2000` bytes) e sugere "Profile2" para o próximo perfil.

## Compatibilidade com o Dolphin (testada em 2026-10-22, Dolphin 2609a)

- Port → Dolphin: um perfil criado pelo port, copiado para `GC/USA/Card A` com o slot A em modo pasta GCI, aparece no Dolphin ("Profiles Saved: 1", Profile1 na lista).
- Dolphin → port: um perfil criado no Dolphin aparece na tela de perfis do port junto com o do port.
- Apagar no port um save lido de um arquivo com nome qualquer remove esse arquivo.

Para repetir o teste sem mexer na configuração do Dolphin, use uma pasta de usuário separada (`Dolphin.exe -u <pasta>`) com `[Core] SlotA = 8` no `Dolphin.ini`. Teclas simuladas só chegam ao Dolphin com o scan code preenchido, porque a entrada dele é por DirectInput.

`-v` mostra no log as aberturas, leituras e gravações (`[card] ...`).

## SRAM

A SRAM do relógio (idioma, modo de som, deslocamento da tela) não é emulada. `OSGetSoundMode` devolve estéreo, e o idioma fica no padrão do SDK (inglês).
