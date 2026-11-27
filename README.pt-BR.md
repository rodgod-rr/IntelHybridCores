# IntelHybridCores

**Núcleos P e E de verdade no macOS Intel (Alder Lake / Raptor Lake)** · [English](README.md)

O macOS para Intel nunca rodou num Mac com núcleos híbridos, então trata todos os núcleos de um Alder Lake ou Raptor Lake como iguais: tarefas de fundo disputam os núcleos P com o app em primeiro plano, e nenhuma ferramenta mostra quais núcleos são de eficiência. Mas o kernel Intel já carrega, sem uso, quase tudo que o Apple Silicon usa para separar os núcleos. O IntelHybridCores é um plugin do [Lilu](https://github.com/acidanthera/Lilu) que preenche essas peças.

| O quê | Como fica |
|---|---|
| Escalonamento por QoS | Tarefas de fundo (Spotlight, Time Machine, análise de fotos) nos núcleos E; primeiro plano nos P |
| `hw.nperflevels` / `hw.perflevel0/1` | `2` / "Performance" e "Efficiency", com núcleos e caches reais |
| Monitor de Atividade → Histórico da CPU | "Núcleo 1 — Desempenho" … "Núcleo 13 — Eficiência" |
| HyperThreading dos núcleos P | Primeiro plano evita a thread irmã de um núcleo P já ocupado (como o ITMT do Linux) |
| `hw.physicalcpu` / Informações do Sistema | Núcleos reais (10 em vez de 16) e "Hyper-Threading: Ativado" |
| Bateria ↔ tomada | Troca de modo sozinha (ex.: 4 na bateria, 2 na tomada) |

> **Atenção:** os offsets do kernel foram tirados do xnu-12377 e conferidos no **macOS 26.7.2 (25G332)** e **26.7.3 (25G342)**, num i5-13450HX (6P + 4E). Cada gancho só é instalado se as instruções do kernel baterem byte a byte; em outra versão do macOS, a parte que não bater fica desligada (ver `sysctl debug.hc_log`). Testado aqui num i5-13450HX; há um relato de funcionamento num i9-13900H (6P + 8E) na [issue #1](https://github.com/rodgod-rr/IntelHybridCores/issues/1) e num i7-12700F (8P + 4E, Alder Lake de desktop) na [issue #2](https://github.com/rodgod-rr/IntelHybridCores/issues/2). Use por sua conta e com um pendrive de socorro à mão.

## Resultados (i5-13450HX, macOS 26.7.2)

Uso por tipo de núcleo, medido com [`tools/cpuuse.c`](tools/cpuuse.c) (100% = um núcleo inteiro ocupado), Mac parado na tomada:

| Cenário | Sem IntelHybridCores (P/E) | Modo 2 (P/E) | Modo 4 (P/E) |
|---|---|---|---|
| 4 laços de fundo | 512% / 30% | 141% / 358% | 95% / 400% |
| 8 laços de fundo | 794% / 147% | 522% / 388% | 95% / 400% |
| 4 de primeiro plano + 8 de fundo | 982% / 316% | 912% / 374% | 544% / 400% |
| 1 laço de primeiro plano | 194% / 0% | 250% / 2% | 192% / 3% |

Sem o IntelHybridCores, o kernel escolhe o CPU livre de menor número, que é sempre um P; os núcleos E quase não trabalham.

Consumo com 8 laços de fundo por 20 s (potência do pacote da CPU):

| | Tomada | Bateria | Trabalho por joule |
|---|---|---|---|
| Modo 2 | 52 W | 30 W | referência |
| Modo 4 | 27 W | 18 W | ~30% menor |

O modo 4 corta a potência pela metade quando há fundo pesado (menos calor e ventoinha, núcleos P livres), mas o fundo demora mais e gasta um pouco mais de energia no total: os núcleos E do Intel em turbo não são tão mais eficientes quanto os do Apple Silicon. Parado, os dois modos ficam iguais (~9,5 W na bateria).

## Modos

`sysctl debug.hc_mode=N` muda na hora; o boot-arg `hcmode=N` define o modo do boot (padrão **2**).

| Modo | Efeito |
|---|---|
| 0 | desligado |
| 1 | fundo e manutenção preferem os núcleos E |
| 2 | 1 + todo o resto prefere os P; fundo que estiver num P migra quando há um E livre |
| 3 | 2 + `utility` (downloads, exportações) prefere os E |
| 4 | 3 + fundo **só** nos E, como no Apple Silicon |

Threads de tempo real (áudio, vídeo) nunca são mexidas. Uma tarefa de fundo promovida acima da prioridade 20 (segurando uma trava de um app em primeiro plano) escapa do modo 4, para não travar esse app.

**Bateria ↔ tomada:** o boot-arg `hcpower=B,T` (ex.: `hcpower=4,2`) usa o modo B na bateria e T na tomada; `sysctl debug.hc_power="4,2"` liga na hora e `"0"` desliga. O modo só muda quando a fonte muda, então um `debug.hc_mode` manual vale até o próximo plugar/desplugar. Se o modo 4 não estiver disponível, cai para o 3.

**HyperThreading:** ligado por padrão; `sysctl debug.hc_smt=0/1` na hora, boot-arg `-hcsmtoff` no boot.

**Throughput (desligado por padrão):** `sysctl debug.hc_tput=1` na hora, boot-arg `-hctput` no boot. O primeiro plano pega primeiro uma thread primária livre de núcleo P, depois um núcleo E, e só então a irmã de HyperThreading de um núcleo P ocupado, então trabalho em lote com muitas threads (compilação, renderização) usa todos os núcleos físicos: um núcleo E livre rende mais que a irmã de um núcleo P que divide a execução com a outra ocupada. Uma thread sozinha continua pegando um núcleo P livre. Precisa do HyperThreading tratado (`smt` no `hc_stat`). Medido no i5-13450HX (6P + 4E, modo 2, na tomada), 10 cópias de um laço de CPU fixo: curto (~10 s) 9,2-10,3 s desligado contra 8,1-8,5 s ligado (~15% mais rápido); sustentado (~30 s, limitado por potência/temperatura) 36,4 s contra 34,1 s na média de 4 rodadas (~6% mais rápido, com variação); ~1 min com ventoinha a 100%: 73,3 s contra 72,0 s (dentro do ruído). O ganho é maior em rajadas curtas, enquanto a CPU ainda roda acima do limite de potência sustentado; quando o pacote fica limitado por potência, em quais núcleos o trabalho roda pesa pouco neste chip de notebook (um chip de desktop, com limite sustentado maior, pode manter mais do ganho). Parcela dos núcleos E nas escolhas do primeiro plano 9-13% -> 27-32%. Uma cópia sozinha: ~30 s 32,4-32,8 s desligado contra 30,3-33,1 s ligado; ~1 min 65,4-68,0 s contra 64,4-71,1 s (sem penalidade). Desligado por padrão porque muda onde o primeiro plano roda; ligue em sessões de build/render.

**Totais de núcleos** (`sysctl debug.hc_perf`, boot-arg `hcperf=N`):

| Valor | `hw.perflevel` e rótulos | `hw.physicalcpu(_max)` | `machdep.cpu.core_count` / `cores_per_package` / `logical_per_package` |
|---|---|---|---|
| 0 | do kernel | do kernel | do kernel |
| 1 | corrigidos | núcleos reais | núcleos reais / threads |
| **2** (padrão) | corrigidos | núcleos reais | do kernel |
| 3 | corrigidos | do kernel | núcleos reais / threads |

O padrão é 2 por causa do "Sobre este Mac" (ver Armadilhas).

Outros boot-args: `-hcoff` (desliga tudo), `-hcschedoff` (sem os ganchos do escalonador), `-hcperfoff` (sem `hw.perflevel`, totais e rótulos).

## Instalação

1. Requisitos: Lilu 1.6.4 ou mais novo; CPU híbrida; **CpuTopologyRebuild desligado ou com `ctrsmt=off`** (só `off`: o modo padrão e o `ctrsmt=full` não servem). Neles, os núcleos E viram threads SMT dos P e a preferência deixa de funcionar (o IntelHybridCores detecta isso e não instala o escalonador; `debug.hc_stat` mostra `state 6`).
2. Copie `IntelHybridCores.kext` para `EFI/OC/Kexts` e adicione em `Kernel → Add` depois do Lilu.
3. Reinicie e confira:

```bash
sysctl debug.hc_stat debug.hc_log hw.nperflevels hw.perflevel1.name hw.physicalcpu debug.hc_smt debug.hc_power
```

`state 1` = escalonador instalado; `strict-ok` = modo 4 disponível; `smt` = HyperThreading tratado; `auto` = troca bateria/tomada ligada; `tput` = throughput ligado. Os contadores mostram quantas escolhas foram para o tipo pedido (`bg E/P`, `fg P/E`) e, no primeiro plano em núcleos P, quantas foram para uma thread primária (`smt prim`), para uma secundária com a irmã livre (`sec-free`) ou ocupada (`sec-cont`). Escrever qualquer valor (`sysctl debug.hc_stat=0`) zera os contadores, para medir uma carga só, como um build.

Binário de referência 0.3.0 (testado no macOS 26.7.3 / 25G342): [`dist/`](dist). A 0.3.0 traz a opção throughput (`hc_tput`) e o zerar do `hc_stat`; a 0.2.0 (sem elas) e a 0.1.0 (sem HyperThreading, totais nem `hcpower`) ficam no histórico do git. Para compilar: `DEPS=/pasta/com/MacKernelSDK+Lilu.kext bash build.sh`.

## Como funciona (e como reproduzir em outra versão)

Tudo está em [`src/IntelHybridCores.cpp`](src/IntelHybridCores.cpp). Referências ao código do xnu: [apple-oss-distributions/xnu](https://github.com/apple-oss-distributions/xnu), tag `xnu-12377.121.6`.

### 1. Tipo de cada núcleo
CPUID `0x1A` em cada CPU (`mp_rendezvous_no_intrs`): `0x40` = Core (P), `0x20` = Atom (E). CPUID `0xB` dá o x2APIC (núcleo físico), e CPUID `4` dá os caches e quantos CPUs dividem o L2.

### 2. Escalonador
O escalonador do Intel é o `dualq`, que usa `choose_processor_smt` (`sched_prim.c`). Essa função já consulta duas máscaras por pset que, no Intel, ficam sempre zeradas, porque só o controlador de desempenho do ARM as escreve:

- `perfcontrol_cpu_preferred_bitmask` (pset+`0x8a0`): entre os CPUs livres, tenta estes primeiro;
- `perfcontrol_cpu_migration_bitmask` (pset+`0x8a8`): CPUs a evitar, inclusive a dica do último CPU da thread.

O gancho preenche as duas conforme a QoS efetiva da thread (`proc_get_effective_thread_policy(thread, TASK_POLICY_QOS)`), chama o original e zera de novo. Roda com o lock do pset, e qualquer valor é válido para o kernel. Para achar os offsets em outra versão: desmonte `_choose_processor_smt` e procure o `and` da máscara de CPUs ociosos com `0x8a0(%r13)`, seguido do `cpu_preferred_last_chosen` (+`0x10`).

O modo 4 precisa de mais dois ganchos, porque um núcleo P livre puxa da fila comum do `dualq` sem passar pela escolha de CPU:

- **`sched_dualq_choose_thread`**: um P recusa uma thread de fundo da fila. A função é estática; o endereço sai da tabela `_sched_dualq_dispatch` (+`0x30`), conferida pelo `choose_processor` em +`0x58`.
- **`sched_dualq_thread_avoid_processor`** (+`0xf0`): uma thread de fundo que já roda num P é tirada de lá. O kernel a recoloca na fila com `SCHED_REBALANCE`.

### 2b. HyperThreading
O kernel monta a topologia pelos campos "por pacote" do CPUID (folhas 1 e 4). Nos híbridos eles trazem o máximo de ids endereçáveis (`machdep.cpu.logical_per_package` = `cores_per_package` = 128), então o kernel conclui 1 thread por núcleo: cada thread HT vira um núcleo próprio, primário de si mesmo (`processor_primary`), e o `choose_processor_smt` não sabe quais CPUs são irmãs. Essa topologia é montada antes de qualquer kext e fica como está. O IntelHybridCores agrupa os núcleos P pelo id de núcleo do CPUID `0xB` (o menor número de CPU de cada núcleo físico é a primária) e, para o primeiro plano, tira da máscara preferida e põe na de migração cada secundária cuja irmã está despachando ou rodando (`processor->state` ≥ 5).

### 3. `hw.perflevel` e totais
O `kern_mib.c` já registra `hw.perflevel0` **e** `hw.perflevel1`; eles só respondem "1 tipo" porque as funções de máquina do Intel dizem isso. O IntelHybridCores troca o `oid_handler` desses nós (achados a partir de `_sysctl__children`) por um que responde por tipo. Do mesmo jeito troca `hw.physicalcpu(_max)` e `machdep.cpu.core_count` / `cores_per_package` / `logical_per_package`, guardando o tratador original (usado com `debug.hc_perf=0`).

### 4. Monitor de Atividade
O Monitor lê o serviço `cpus` do IODeviceTree e, em cada filho, `cluster-type` (dado `"P"`/`"E"`, o byte `'E'` = Eficiência, `'P'` = Desempenho, `'M'` = um terceiro tipo) e `logical-cpu-id`. Depois **ordena todos os filhos por `logical-cpu-id`**. Num PC, os filhos são os processadores ACPI (`C000@0`, `C00C@38`… o endereço é o x2APIC), e o ACPI costuma declarar mais processadores do que existem; os vazios recebem ids 64+ para irem para o fim da lista.

### 5. Bateria ↔ tomada
O serviço da bateria (`IOPMPowerSource`, ex.: `AppleSmartBattery`) é publicado uma vez só no boot: a notificação de publicação **não** dispara de novo ao plugar ou desplugar. O IntelHybridCores guarda o serviço e lê `ExternalConnected` a cada 5 s (`thread_call`).

### Armadilhas (cada uma custou um panic ou um teste falho)

- **"Sobre este Mac" mostra o nome pelo `machdep.cpu.core_count`.** Ele junta o tipo de processador do SMBIOS com esse total e procura numa tabela de CPUs que a Apple vendeu. Com 16 "núcleos" não acha nada e mostra o nome real do CPUID; com o tipo `0x0F01` (Xeon W, comum em configs do OpenCore) e 10 núcleos ele acha o "Xeon W 10-Core" do iMac Pro. Por isso o padrão (`hc_perf=2`) corrige `hw.physicalcpu`, que os programas usam, e deixa o `machdep.cpu.core_count` do kernel. Trocar o `ProcessorType` para um código de Core i5 num SMBIOS de MacBookPro16,1 (que só teve i7/i9) dá CPU "desconhecida".

- **O kernel do Tahoe é compilado com LTO.** `ml_get_cpu_types`, `ml_get_cpu_number_type` e afins foram embutidas no `sysctl_hw_generic` como constantes: desviar essas funções não muda nada. Por isso o IntelHybridCores troca o tratador das sysctls.
- **As sysctls estáticas ficam em `__DATA_CONST`**, que vira somente leitura depois do boot: gravar o `oid_handler` direto dá **panic**. Grave com `MachInfo::setKernelWriting` do Lilu.
- **Argumentos não usados não são passados.** O `thread_select` chama o `choose_thread` só com `(processor, priority)`, porque o original ignora o resto; o terceiro argumento (`prev`) chega com lixo.
- **Recusar uma thread exige `must_idle`.** Se o `choose_thread` devolver nada com a fila cheia, o `thread_select` dá volta sem fim. Marque `processor->must_idle` (+`0x71`), a mesma saída que o kernel usa para threads NO_SMT.
- **Funções que começam com salto relativo** (`ml_cpu_get_info_type`, `ml_cpu_cache_sharing`, `ml_get_cpu_number_type`) não servem para o trampolim do Lilu.
- **No zsh, `log` é um comando interno.** Para ler o log do sistema, use `/usr/bin/log show`.
- O log de kexts injetados pelo OpenCore costuma não aparecer no `log show`; por isso o IntelHybridCores guarda as últimas mensagens em `sysctl debug.hc_log`.

## Licença e créditos

MIT, © 2026 Rodrigo Lacerda Rangel Santos. Usa o [Lilu](https://github.com/acidanthera/Lilu) e o [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) (acidanthera). Nomes e estruturas do kernel vêm do código aberto do [xnu](https://github.com/apple-oss-distributions/xnu). Sem relação com a Apple ou a Intel.
