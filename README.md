# Задание 5: Безопасное копирование и хранение ключа

## Сборка

Linux:

    make

Windows (MSYS2 / MinGW):

    mingw32-make

## Запуск

Старый режим:

    ./secure_copy input.txt output.bin 42

Sequential:

    ./secure_copy --mode=sequential --key=42 file1.txt file2.txt file3.txt

Parallel:

    ./secure_copy --mode=parallel --key=42 file1.txt file2.txt file3.txt file4.txt file5.txt

Auto:

    ./secure_copy --mode=auto --key=42 file1.txt file2.txt file3.txt file4.txt file5.txt

## Тесты

Старый тест:

    make test

Sequential тест:

    make test-seq

Parallel тест:

    make test-par

Auto тест:

    make test-auto

Демонстрация на 10 файлах:

    make demo10

Большой тест:

    make bigtest

Очистка:

    make clean

## Ручная проверка

Создать файлы:

    echo "Alpha" > f1.txt
    echo "Beta" > f2.txt
    echo "Gamma" > f3.txt
    echo "Delta" > f4.txt
    echo "Epsilon" > f5.txt
    echo "Zeta" > f6.txt

Sequential:

    ./secure_copy --mode=sequential --key=42 f1.txt f2.txt f3.txt

Parallel:

    ./secure_copy --mode=parallel --key=42 f1.txt f2.txt f3.txt f4.txt f5.txt f6.txt

Auto:

    ./secure_copy --mode=auto --key=42 f1.txt f2.txt f3.txt f4.txt f5.txt f6.txt

Расшифровать и проверить:

    ./secure_copy f1.txt.enc f1_restored.txt 42
    cmp f1.txt f1_restored.txt

---

## Практическое задание № 5: Безопасное хранение ключа шифрования

В проекте реализовано защищенное хранение ключа в оперативной памяти с использованием системных вызовов `mmap`, `mprotect` и `sigaction`.

### Алгоритм работы защиты:
1. **Выделение памяти:** Ключ не хранится в обычных переменных. Под него выделяется анонимная страница памяти через `mmap` (`MAP_PRIVATE | MAP_ANONYMOUS`).
2. **Инициализация:** Права устанавливаются на `PROT_READ | PROT_WRITE`, ключ копируется через `memcpy`, после чего доступ полностью блокируется (`PROT_NONE`).
3. **Шифрование:** При вызове функции шифрования права временно расширяются только до чтения (`PROT_READ`), а после завершения операции снова аннулируются (`PROT_NONE`).
4. **Обработка нарушений (SIGSEGV):** Установлен собственный обработчик сигнала `SIGSEGV` через `sigaction`. Если происходит попытка записи в защищенную область ключа, программа перехватывает сбой, выводит сообщение об ошибке безопасности и завершается с кодом 139.
5. **Затирание ключа:** При завершении работы (успешном или при прерывании) функция `cleanup_key` временно возвращает права на запись, заполняет страницу нулями с помощью `memset`, закрывает доступ и освобождает память через `munmap`.

### Демонстрация защиты памяти

Для демонстрации работы защиты (попытки несанкционированной модификации ключа) используйте специальный флаг `--test-violation`:

    ./secure_copy --test-violation --mode=sequential --key=42 f1.txt

Программа инициирует запись в защищенную память и корректно отловит `Segmentation fault` с выводом соответствующего предупреждения.