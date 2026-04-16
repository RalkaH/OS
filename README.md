# Задание 4.

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