# Fases

A tabela de fases fica em `0x803C93C0` no `main.dol`. São 57 entradas de 44 bytes: ponteiro para o título, mundo, índice da missão, mundo de novo, script da missão (`ms_*`), ponteiros de funções especiais (só na fase 42) e dois floats. `Level_FindIndex` (`0x801B0A74`) procura uma entrada pelo título, e o carregador `fn_801B0AE8` guarda o índice em `0x804B891C` (-1 fora de uma fase).

## Testar uma fase direto

`MA_LEVEL=<entrada>` troca a primeira fase carregada pela entrada pedida. Ela vale tanto ao começar a campanha (Start Adventure) quanto no multijogador. Exemplo, a partir da tela título com um perfil novo:

```bash
MA_LEVEL=21 MA_SKIP_MOVIES=1 MA_SAVE_DIR=build/test_saves \
  MA_INPUT="300:START,450:START,600:START,750:START,900:A,1000:DOWN,1100:A,1250:A,1400:A" ./build/metalarms.exe
```

A entrada 0 é uma fase de teste dos desenvolvedores (`Level01`) que não está no disco; pedir ela faz o próprio jogo mostrar a mensagem de disco com problema. É uma ferramenta de desenvolvimento: a fase começa sem o inventário nem o estado de missão que o jogo teria ao chegar nela jogando. O log mostra `[fase] carregando ...` a cada fase.

## Entradas

| Entrada | Título | Mundo | Script |
|---|---|---|---|
| 0 | Generic | `Level01` | `ms_gtest_01` |
| 1 | 1 Seal the Mines | `WEDMmines01` | `ms_mines01` |
| 2 | 2 Seal the Mines | `WEDMmines02` | `ms_mines02` |
| 3 | 3 Seal the Mines | `WEDMmines03` | `ms_mines03` |
| 4 | 4 Clean Up | `WEDTtown_01` | `ms_town_01` |
| 5 | 5 RAT race | `WEWTrace_01` | `ms_race_01` |
| 6 | 6 Wasteland Journey | `WEWJjourn01` | `ms_journ01` |
| 7 | 7 Wasteland Journey | `WEWJjourn02` | `ms_journ02` |
| 8 | 8 Wasteland Journey | `WEWJjourn03` | `ms_journ03` |
| 9 | 9 ZombieBot Boss | `WEWZzombi01` | `ms_zombi01` |
| 10 | 10 Destroy Comm Cntr | `WEWCcomm_01` | `ms_comm_01` |
| 11 | 11 Destroy Comm Cntr | `WEWCcomm_02` | `ms_comm_02` |
| 12 | 12 Destroy Comm Cntr | `WEWCcomm_03` | `ms_comm_03` |
| 13 | 13 Hold Your Ground | `WEWChold_01` | `ms_hold_01` |
| 14 | 14 R & D | `WEWRresrch1` | `ms_resrch1` |
| 15 | 15 R & D | `WEWRresrch2` | `ms_resrch2` |
| 16 | 16 R & D | `WEWRresrch3` | `ms_resrch3` |
| 17 | 17 R & D | `WEWRresrch4` | `ms_resrch4` |
| 18 | 18 Wasteland Chase | `WEWHchase01` | `ms_chase01` |
| 19 | 19 Morbot Region | `WERMmorbot1` | `ms_morbot1` |
| 20 | 20 Morbot Region | `WERMmorbot2` | `ms_morbot2` |
| 21 | 21 Reactor | `WERRreactr1` | `ms_reactr1` |
| 22 | 22 Reactor | `WERRreactr2` | `ms_reactr2` |
| 23 | 23 Mil City Hub | `WEMCcity_01` | `ms_city_01` |
| 24 | 24 Mil City Hub | `WEMCcity_03` | `ms_city_03` |
| 25 | 25 Spy vs. Spy | `WECFfacty01` | `ms_facty01` |
| 26 | 26 Ruins | `WEMCcity_05` | `ms_city_05` |
| 27 | 27 Ruins | `WECRruins01` | `ms_ruins01` |
| 28 | 28 Ruins | `WECRruins02` | `ms_ruins02` |
| 29 | 29 Secret Rendezvous | `WEMCcity_02` | `ms_city_02` |
| 30 | 30 Night Sneak | `WECDsneak01` | `ms_sneak01` |
| 31 | 31 Night Sneak | `WECDsneak02` | `ms_sneak02` |
| 32 | 32 Invasion | `WEDiinvas01` | `ms_invas01` |
| 33 | 33 Coliseum | `WEBCcolis01` | `ms_colis01` |
| 34 | 34 Coliseum | `WEBCcolis02` | `ms_colis02` |
| 35 | 35 Coliseum | `WEBCcolis03` | `ms_colis03` |
| 36 | 36 Coliseum | `WEBCcolis04` | `ms_colis04` |
| 37 | 37 Race to the Rocket | `WEWKrockt01` | `ms_rockt01` |
| 38 | 38 Space Dock | `WESHhangr01` | `ms_hangr01` |
| 39 | 39 Space Station | `WESSstatn01` | `ms_statn01` |
| 40 | 40 Space Station | `WESSstatn02` | `ms_statn02` |
| 41 | 41 Gen. Corrosive | `WESRrepair1` | `ms_repair1` |
| 42 | 42 Final Battle | `WESCcorros1` | `ms_corros1` |
| 43 | 1 Big E | `WE01multi01` | `ms_mp_01` |
| 44 | 2 Spy v Spy | `WE02multi02` | `ms_mp_02` |
| 45 | 3 Tanks Alot | `WE03multi03` | `ms_mp_03` |
| 46 | 4 MacMines | `WE05multi05` | `ms_mp_05` |
| 47 | 5 Inferno Machine | `WE11multi11` | `ms_mp_11` |
| 48 | 6 Clean Up | `WE15multi15` | `ms_mp_15` |
| 49 | 7 Trenches | `WE08multi08` | `ms_mp_08` |
| 50 | 8 Comm AAGun | `WE12multi12` | `ms_mp_12` |
| 51 | 9 Morbotland | `WE14multi14` | `ms_mp_14` |
| 52 | 10 Reactor | `WE09multi09` | `ms_mp_09` |
| 53 | 11 Lost Boss | `WE10multi10` | `ms_mp_10` |
| 54 | 12 Ruins | `WE07multi07` | `ms_mp_07` |
| 55 | 13 Coliseum | `WE04multi04` | `ms_mp_04` |
| 56 | 14 Corrosive City | `WE06multi06` | `ms_mp_06` |

## Varredura automática

`bash tools/test_levels.sh <primeira> <última> <segundos>` abre cada entrada pela campanha, anda um pouco e grava em `build/levels/` as capturas, o log e um `resumo.txt` (queda, fps, avisos de recurso do GX não emulado). `python tools/levels_sheet.py <primeira> <última> <saída.png>` monta uma folha com a última captura de cada fase.

Resultado em 2026-10-22, entradas 1 a 42 (a campanha inteira): todas carregam e renderizam, sem nenhuma queda, a 59–60 quadros por segundo e sem nenhum aviso de recurso não emulado. Várias terminam em "Mission Failed" porque o roteiro de botões é genérico, e não por problema do port. As entradas 43 a 56 (as 14 arenas do multijogador) também passaram, a 60 quadros por segundo. As folhas de captura ficam em `build/levels/sheet_*.png`.
