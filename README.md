# Atlas Task Engine

Biblioteca de execução concorrente em **C++20**, com fila limitada por capacidade, prioridades, tarefas com resultados tipados, cancelamento cooperativo e métricas consistentes. Inclui uma CLI que demonstra o motor processando trabalho real de CPU: contagem de números primos.

Projeto de portfólio avançado, versão **0.1.0**. O objetivo é exercitar decisões de engenharia de C++ moderno e concorrência por meio de um componente reutilizável, testes e contratos explícitos. Não depende de frameworks ou serviços externos.

## Por que este projeto é avançado

| Conceito | Aplicação no código |
| --- | --- |
| RAII e ownership | Motor possui as threads; tarefas possuem seus argumentos; recursos são liberados por destrutores. |
| Move semantics | Callables, argumentos e resultados podem ser move-only, inclusive `std::unique_ptr`. |
| Templates, concepts e perfect forwarding | `submit` verifica invocabilidade e preserva a transferência dos argumentos. |
| Type erasure | Interface interna `Job` permite armazenar tarefas heterogêneas por `unique_ptr`, sem exigir callables copiáveis. |
| Concorrência C++20 | `std::jthread`, `std::stop_source`, `std::stop_token`, mutexes e condition variables. |
| Resultados assíncronos | `std::promise` e `std::future` transportam valores, `void` e exceções. |
| Heap de prioridades | Seleção por prioridade com desempate FIFO na retirada da fila. |
| Backpressure | Rejeição imediata ou submissão com espera limitada quando a fila está cheia. |
| Ciclo de vida | Encerramento por drenagem ou cancelamento; threads são aguardadas antes da destruição. |
| Reentrância e deadlocks | Guardas para operações bloqueantes chamadas pelos próprios workers; callbacks e destrutores fora do mutex do estado. |

## Funcionalidades

- Biblioteca estática `atlas` e alvo CMake `atlas::atlas`.
- Número de workers e capacidade da fila configuráveis.
- Três prioridades: `low`, `normal` e `high`.
- Resultados tipados, tarefas `void` e propagação de exceções, inclusive exceções não derivadas de `std::exception`.
- Cancelamento individual por handle e solicitação de cancelamento de todo o motor.
- `close`, `shutdown` e `wait_idle` com comportamentos documentados.
- Métricas de tarefas aceitas, concluídas, falhas, cancelamentos, rejeições, pendências e execução.
- CLI com argumentos validados, saída textual ou JSON e demonstração de cancelamento.
- 23 casos funcionais, cenário de estresse com 10.000 tarefas e testes de integração pelo CTest.
- Configuração de ASan/UBSan e TSan em builds independentes.
- Workflow GitHub Actions para Windows/Linux e verificação com ASan/UBSan.

## Requisitos

- Compilador **e biblioteca padrão** com C++20, `std::jthread`, `std::stop_token`, espera interrompível e `std::latch`.
- CMake **3.24+**.
- Ninja, se escolher o gerador `Ninja`.
- Referência local validada: GCC/libstdc++ 13.3 no Linux. MSVC recente do Visual Studio 2022 e GCC recente do MSYS2 UCRT64 são opções para Windows.

No macOS, a disponibilidade também depende da versão de libc++ do toolchain; apenas selecionar `-std=c++20` não acrescenta recursos ausentes da biblioteca. A configuração verifica `jthread`/`stop_token` e apresenta erro se esses recursos faltarem. Windows, macOS e compiladores diferentes do GCC local ainda dependem de verificação no ambiente de destino.

## Obter o código

```bash
git clone https://github.com/eliasramineli-sys/Atlas-Task-Engine.git
cd Atlas-Task-Engine
```

O acesso ao clone depende das permissões do repositório.

## Windows com MSYS2 UCRT64

Abra o terminal **MSYS2 UCRT64**, usando uma instalação atualizada. Se as ferramentas ainda não estiverem instaladas:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

Na pasta extraída `atlas-task-engine`, confira e compile:

```bash
g++ --version
cmake --version
ninja --version
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/atlas-cli.exe --workers 4 --jobs 24 --limit 50000 --json
```

Use o CMake e o compilador do mesmo ambiente UCRT64. Não misture bibliotecas/compiladores de ambientes MSYS, UCRT64 e instalações MinGW independentes.

## Windows com Visual Studio 2022

Em um Developer PowerShell com as ferramentas de C++ instaladas:

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release --parallel
ctest --test-dir build-msvc -C Release --output-on-failure
.\build-msvc\Release\atlas-cli.exe --workers 4 --jobs 24 --limit 50000 --json
```

## Linux e toolchains compatíveis no macOS

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/atlas-cli --workers 4 --jobs 24 --limit 50000 --json
./build/atlas-example
```

Não há executável Windows incluído ou instalador. O ZIP entrega código-fonte e configuração de build; o executável é gerado no seu computador. O desenvolvimento do projeto não requer Python.

## Usar a biblioteca

```cpp
#include <atlas/engine.hpp>
#include <memory>

atlas::Engine engine({.workers = 4, .capacity = 128});

auto task = engine.submit(atlas::Priority::high,
    [](std::stop_token stop, std::unique_ptr<int> value) {
        atlas::cancellation_point(stop);
        return *value * 2;
    }, std::make_unique<int>(21));

int answer = task.get(); // 42; bloqueia até a conclusão ou relança a exceção.
engine.shutdown(atlas::Shutdown::drain);
```

O primeiro argumento de todo callable é `std::stop_token`. Os demais argumentos são armazenados por valor com decay e movidos para a invocação. Use `std::ref` para empréstimos explícitos e mantenha o objeto referenciado vivo até a tarefa terminar. Retornos por referência são recusados; retorne um valor próprio ou um smart pointer.

Em outro projeto CMake, adicione este diretório com `add_subdirectory` e vincule seu alvo a `atlas::atlas`. Um exemplo completo está em [examples/usage.cpp](examples/usage.cpp).

## CLI e trabalho demonstrado

```bash
atlas-cli --help
atlas-cli --workers 4 --jobs 32 --queue 64 --limit 50000
atlas-cli --workers 2 --jobs 24 --limit 1000000 --cancel-after-ms 1 --json
```

Cada tarefa conta primos até `--limit`. O checksum soma os resultados das tarefas que concluíram; tarefas canceladas não contribuem. Para `--limit 1000`, cada tarefa concluída retorna 168. A saída informa dados da execução atual; não há alegação de aceleração ou benchmark comparativo.

O timer de cancelamento começa depois da fase de submissão. Se as tarefas já tiverem terminado, a solicitação não altera os resultados. Os tempos e a quantidade de cancelamentos variam com hardware, carga e escalonamento. As submissões da CLI esperam até 5 segundos por espaço; um timeout encerra a demonstração com erro e limpeza do motor.

Faixas: workers 1–64, jobs 1–1.000, fila 1–100.000, limite de primos 2–2.000.000 e cancelamento 1–10.000 ms. Saídas do processo: `0` = execução consistente sem falhas de tarefa, inclusive cancelamentos esperados; `1` = erro do motor/tarefa; `2` = argumentos inválidos.

## Contratos de concorrência

- A capacidade conta tarefas **pendentes**, e não as que já estão executando. Ela limita a quantidade de registros na fila, não a memória arbitrária capturada pelos callables.
- Prioridade e FIFO se aplicam à retirada da fila; tarefas já em execução não sofrem preempção. Com vários workers, a ordem de conclusão pode ser diferente. Fluxo contínuo de alta prioridade pode adiar tarefas de baixa prioridade.
- `submit` lança `queue_full` quando não há espaço. `submit_for` pode lançar `queue_timeout` ou `engine_closed`; a construção do callable também pode falhar antes da aceitação.
- `request_cancel()` solicita parada; não mata threads nem descarta automaticamente um resultado já produzido. Uma tarefa pendente cancelada permanece na fila e sua future é resolvida quando um worker a retira, ou quando `close(cancel)` esvazia a fila.
- Tarefas ativas devem consultar o token e cooperar. `cancellation_point` lança `task_cancelled`. Uma tarefa que retorna normalmente depois da solicitação conta como sucesso.
- `close(drain)` recusa novas submissões e deixa as aceitas terminarem. `close(cancel)` resolve as tarefas pendentes como canceladas e solicita parada das ativas.
- `shutdown` também aguarda as threads; o destrutor usa cancelamento seguido de join. Uma tarefa que ignora o token pode atrasar esse encerramento indefinidamente.
- Workers deste motor não podem chamar `wait_idle` ou `shutdown`. Esperar por espaço na própria fila cheia também é recusado. Esperar uma future dependente de tarefas do mesmo pool ainda pode causar esgotamento dos workers: organize dependências externamente.
- O motor deve sobreviver a todas as chamadas de seus produtores. Pare e aguarde os produtores antes de destruir o objeto. Não destrua o motor dentro de uma de suas próprias tarefas.
- Callbacks de parada e destrutores de objetos capturados devem ser não bloqueantes e não aguardar o próprio motor. Uma callback de parada não pode deixar exceções escaparem.
- Um handle descartado não cancela a tarefa. Handles e futures podem sobreviver ao motor, mas não estendem a execução das threads dele.

`wait_idle` observa fila e execução vazias naquele momento; não impede submissões futuras. Uma future pode ficar pronta antes da contabilização final do worker. Para métricas finais, aguarde `wait_idle` ou `shutdown` e encerre os produtores.

## Métricas e arquitetura

O snapshot é obtido sob o mesmo mutex que protege as transições. A invariável verificada nos testes é:

```text
accepted = queued + running + succeeded + failed + cancelled
```

Rejeições por fila cheia, timeout, motor fechado ou espera inválida de worker não entram em `accepted`. Erros de validação, construção/alocação e esgotamento de IDs não são tratados como trabalhos aceitos nem como rejeições de capacidade.

Leia [docs/ARQUITETURA.md](docs/ARQUITETURA.md) para os estados e decisões de ownership e [docs/VALIDACAO.md](docs/VALIDACAO.md) para os resultados realmente observados.

## Sanitizadores

Em Linux com GCC/Clang compatível, use builds separados:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DATLAS_SANITIZE_ADDRESS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DATLAS_SANITIZE_THREAD=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

O suporte desses runtimes depende do sistema operacional e de suas permissões. A execução local de ASan/UBSan precisou de `ASAN_OPTIONS=detect_leaks=0` porque o LeakSanitizer não pôde consultar os processos no ambiente. Essa adaptação não é aplicada ao CI e não representa validação de vazamentos. Consulte a validação para o resultado final de cada ferramenta.

## Estrutura

```text
atlas-task-engine/
  include/atlas/engine.hpp   API, handles, concepts e modelo tipado de tarefa
  src/engine.cpp            fila, workers, métricas e ciclo de vida
  app/main.cpp              demonstração de trabalho de CPU e JSON
  examples/usage.cpp        exemplo de integração
  tests/engine_tests.cpp    casos funcionais e estresse concorrente
  docs/                     arquitetura e evidências de validação
  CMakeLists.txt            biblioteca, executáveis, CTest e sanitizadores
  .github/workflows/ci.yml   configuração de verificação remota
```

## Roteiro para estudar o código

1. Compile e execute `atlas-example`; acompanhe um resultado e uma exceção.
2. Leia `TaskHandle` e `JobModel`: propriedade de objetos, decay, forwarding e retorno tipado.
3. Siga `enqueue` e o heap: aceitação, capacidade, prioridade e ordem FIFO.
4. Leia `worker_loop`: transferência de ownership, execução sem o mutex e contabilização.
5. Acompanhe os testes de cancelamento, reentrância e encerramento concorrente.
6. Execute o estresse e os sanitizadores antes de alterar o escalonador.

## Escopo e publicação

Não há persistência, execução distribuída, work stealing, dependências entre tarefas, prioridade com aging ou execução em tempo real. O projeto não foi validado como infraestrutura de produção nem submetido a benchmark comparativo. As garantias de sucesso ou cancelamento dependem dos contratos acima.

Repositório: [eliasramineli-sys/Atlas-Task-Engine](https://github.com/eliasramineli-sys/Atlas-Task-Engine). Consulte a aba [Actions](https://github.com/eliasramineli-sys/Atlas-Task-Engine/actions) para os resultados de CI de cada commit; as evidências locais estão em [docs/VALIDACAO.md](docs/VALIDACAO.md).

A licença de reutilização permanece pendente de escolha; nenhuma foi imposta. Os arquivos de build, ambientes e binários não devem ser versionados.

## Referências técnicas

- [C++ — especificação de stop tokens e callbacks](https://eel.is/c++draft/thread.stoptoken)
- [WG21 — proposta de jthread e cancelamento cooperativo](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p0660r9.pdf)
- [CMake — recursos de linguagem e biblioteca](https://cmake.org/cmake/help/latest/prop_gbl/CMAKE_CXX_KNOWN_FEATURES.html)
- [MSYS2 — uso correto do CMake](https://www.msys2.org/docs/cmake/)
- [libc++ — suporte a C++20](https://libcxx.llvm.org/Status/Cxx20.html)
