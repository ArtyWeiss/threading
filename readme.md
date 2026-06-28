# Multithreading and SIMD :: Win32

#### Готовые бинарники: https://github.com/ArtyWeiss/threading/releases

### Как собрать и запустить?
```
1. Запустить `Developer PowerShell for VS *` (cl.exe должен быть виден из консоли, проверка: `> cl`)
2. Запустить `./build.bat`
3. Запустить `./build/threading.exe` (лучше из консоли, иначе не посмотреть результаты)

P.S. Параметры в начале `threading.cpp`
```

### Идея
Есть массив радномных кругов. Нужно найти все пересекающиеся пары.
Задача делится на блоки и распределяется между несколькими тредами с помощью неблокирующей очереди.
Вычисления ускорены с помощью SIMD (на x86_64 круги считаются по 8 за раз, на arm64 по 4 за раз).
Метрики со всего этого дела снимаются профайлером: [результаты](#результаты)

#### P.S. Сознательно срезаны некоторые углы, чтобы не раздувать скоуп:

1) Воркеры и очередь одноразовые. Т.е. для того, чтобы задачи можно было докидывать в процессе, нужно доработать очередь до кругового тред-сейф буфера (read+write head через atomic_u32). А при пустой очереди воркеры не должны завершать цикл. Вместо этого они должны блочиться об семафор, который будет звонить на каждом пополнении списка задач.

2) Проверка пересечения всех со всеми - это крайне наивное решение задачи. Более оптимальным вариантом будет пространственное разделение для уменьшения количества проверок. Но основной смысл демки не в эффективности решения, а в его производительности, что достигается с помощью трединга и SIMD.

3) У профайлера нет функции для сброса таймингов, т.к. процесс замера линейный и конечный. Для реализации сброса достаточно выполнить atomic.exchange(0) на hits+ticks.

---

#### Очередь задач и воркеры
Задачи хранятся в списке, который можно безопасно заполнять только из мейнтреда.
Брать задачи можно из любого треда, синхронизация сделана через spinlock (треды не блокируются).
Мейнтред тоже делает задачи, пока очередь не опустеет. После чего ждёт, пока все начатые задачи не будут завершены.

#### SIMD
На x86_64 используются AVX с 256-битными регистрами, т.е. в одну операцию помещается 8 элементов.
На arm64 используется NEON, там есть только 128-бит, поэтому проверяется по 4 элемента.
Также есть скалярный фоллбек, который считает элементы по одному.

#### Профайлер
Замеры делаются с помощью макроса PROFILER_SCOPE(name). Он создаёт структуру, деструктор которой записывает замеры в глобальный статический массив, размер которого известен благодаря макросу `__COUNTER__` (вставляется после всех вызовов PROFILER_SCOPE).
Запись значений синхронизируется через atomic.fetch_add(memory_order_relaxed), т.к. порядок не важен.

Макрос можно воткнуть в любой скоуп, в т.ч. внутри уже профилируемого скоупа, например:
```cpp
void test_function() {
    PROFILER_SCOPE("function");
    // ...

    {
        PROFILER_SCOPE("subscope");
        // ...

    } // <- "subscope" will report about this

    // ...
}// <- "function" will report about this
```


## Результаты

#### Параметры
```
RANDOM_SEED 6767
CIRCLE_COUNT 100000
CIRCLE_PER_TASK 8192
WORKER_THREAD_COUNT 5
```

#### X86_64(Intel Core i5-10600KF)
```
======== SIMULATION RESULTS (SIMD) ========
100000 circles
43596117 collisions
======== PROFILER RECORDS ========
threading.cpp:137               [scene_generation]::       1436.5883 ms          1 hits    1436.58825684 ms/hit
threading.cpp:193                        [do_task]::      10603.7754 ms         13 hits     815.67498779 ms/hit // -22%
threading.cpp:200              [circle_collisions]::      10603.1748 ms      12500 hits       0.84820002 ms/hit
threading.cpp:397                 [simulation_run]::       2208.7122 ms          1 hits    2208.71215820 ms/hit // -21%
threading.cpp:403                  [prepare_tasks]::          0.8427 ms          1 hits       0.84270000 ms/hit
```

```
======== SIMULATION RESULTS (SCALAR) ========
100000 circles
43596117 collisions
======== PROFILER RECORDS ========
threading.cpp:137               [scene_generation]::       1441.8800 ms          1 hits    1441.88000488 ms/hit
threading.cpp:193                        [do_task]::      13608.7773 ms         13 hits    1046.82897949 ms/hit
threading.cpp:200              [circle_collisions]::      13604.4863 ms     100000 hits       0.13600001 ms/hit
threading.cpp:397                 [simulation_run]::       2799.4163 ms          1 hits    2799.41625977 ms/hit
threading.cpp:403                  [prepare_tasks]::          0.9571 ms          1 hits       0.95709997 ms/hit
```

#### ARM64(Snapdragon X1 Elite)
```
======== SIMULATION RESULTS (SIMD) ========
100000 circles
43596117 collisions
======== PROFILER RECORDS ========
threading.cpp:137               [scene_generation]::       1936.3455 ms          1 hits    1936.34545898 ms/hit
threading.cpp:193                        [do_task]::      20026.3281 ms         13 hits    1540.48669434 ms/hit // -40%
threading.cpp:200              [circle_collisions]::      20023.8398 ms      25000 hits       0.80089998 ms/hit
threading.cpp:397                 [simulation_run]::       3961.1482 ms          1 hits    3961.14819336 ms/hit // -29%
threading.cpp:403                  [prepare_tasks]::          0.4999 ms          1 hits       0.49990001 ms/hit
```

```
======== SIMULATION RESULTS (SCALAR) ========
100000 circles
43596117 collisions
======== PROFILER RECORDS ========
threading.cpp:137               [scene_generation]::       1930.3827 ms          1 hits    1930.38269043 ms/hit
threading.cpp:193                        [do_task]::      33477.5859 ms         13 hits    2575.19897461 ms/hit
threading.cpp:200              [circle_collisions]::      33464.3984 ms     100000 hits       0.33460000 ms/hit
threading.cpp:397                 [simulation_run]::       6290.7930 ms          1 hits    6290.79296875 ms/hit
threading.cpp:403                  [prepare_tasks]::          0.5317 ms          1 hits       0.53170002 ms/hit
```
