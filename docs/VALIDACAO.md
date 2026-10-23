# Validação observada — 23/10/2026

## Ambiente e escopo

Linux, GCC/libstdc++ **13.3.0**, CMake **4.4.4** e Ninja **1.13.2**. Compilação C++20 e biblioteca padrão do sistema. A verificação foi feita por execução real dos binários, sem dados ou resultados simulados.

As dependências CMake/Ninja pertencem ao ambiente de build e não são dependências de execução da biblioteca. O pacote entregue contém apenas o código, testes, documentação e configuração do projeto.

## Resultados

| Configuração | Resultado observado |
| --- | --- |
| Debug | Cinco grupos do CTest passaram |
| Release | Cinco grupos do CTest passaram; build final sem avisos do compilador |
| ThreadSanitizer | Cinco grupos passaram, incluindo o estresse de 10.000 tarefas, sem diagnóstico de race observado |
| AddressSanitizer + UndefinedBehaviorSanitizer | Cinco grupos passaram com `ASAN_OPTIONS=detect_leaks=0`, sem diagnóstico observado dessas ferramentas |
| LeakSanitizer | Não validado: execução padrão falhou por impossibilidade de consultar `/proc/.../task` no ambiente |
| CLI com carga conhecida | Oito tarefas até 1.000: oito sucessos, nenhuma falha/cancelamento e checksum 1.344 |
| CLI com cancelamento real | Cenário de 24 tarefas até 1.000.000 e timer de 1 ms encerrou com 24 cancelamentos nesta execução |
| Exemplo de integração | Resultado move-only correto e exceção transportada para a future |

O número de cancelamentos e os tempos variam com o escalonamento e a máquina. O cenário de cancelamento não estabelece que todas as tarefas de qualquer carga serão interrompidas. Não foi medido speedup em relação a uma execução sequencial.

## Testes exercitados

O grupo `engine.unit` contém **23 casos funcionais**: valores e `void`, ownership move-only, referências explícitas, exceções e recuperação do worker, prioridade/FIFO, capacidade, timeout, recuperação de produtores, cancelamento pendente/ativo, resultado normal após solicitação de parada, cancelamento no shutdown, drenagem, rejeição após fechamento, guardas de espera do worker, fechamento não bloqueante a partir de uma tarefa, callbacks e destrutores reentrantes, liberação de produtores, shutdown concorrente, RAII, descarte de handle e entradas inválidas.

O grupo `engine.stress` submete **10.000 tarefas**, com quatro produtores, quatro workers, fila de capacidade 64 e uma thread observando métricas durante a execução. Os resultados individuais são somados e comparados com a soma esperada. A invariável de contagem e os limites de fila/execução também são conferidos durante o processamento.

Os outros grupos verificam a CLI com trabalho real, rejeição de argumentos inválidos e o exemplo de integração. As verificações não usam `assert`, portanto continuam ativas em Release.

## Comandos relevantes

```bash
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DATLAS_SANITIZE_THREAD=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DATLAS_SANITIZE_ADDRESS=ON
cmake --build build-asan --parallel
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

ASan e TSan foram executados em diretórios separados. Desativar a detecção de vazamentos foi uma adaptação local; a configuração de CI não a força.

## Limites

- Windows/MSVC, MSYS2 UCRT64 e macOS não foram executados neste ambiente. Há instruções e configuração portáveis, mas isso não constitui uma validação nessas plataformas.
- O workflow GitHub Actions está preparado para Windows/Linux e ASan/UBSan; na validação local inicial, não houve execução remota observada. Resultados posteriores devem ser consultados na aba Actions do repositório para o commit correspondente.
- Não houve teste de fault injection para falha de alocação/criação de thread, benchmark comparativo, teste em tempo real ou avaliação de contenção com muitos núcleos.
- Sanitizadores e testes exercitam interleavings observados; não provam ausência de todos os deadlocks ou races possíveis.
- O cancelamento ativo é cooperativo. Callables que não observam o token podem concluir normalmente ou manter o shutdown aguardando.
- Uso incorreto de referências, callbacks bloqueantes ou destruição do motor de dentro da própria tarefa viola os contratos documentados.

O resultado é um projeto funcional de portfólio avançado, com evidências locais de teste e limites explícitos, sem alegação de prontidão para qualquer ambiente de produção.
