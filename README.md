# Задание 3. Многопоточный файловый копировщик с шифрованием

## Сборка

Linux:
make

Windows (MSYS2):
mingw32-make

## Запуск

./secure_copy_mt file1.txt file2.txt file3.txt output_dir 42

## Особенности

- Многопоточная обработка (3 потока)
- Шифрование (XOR / Caesar)
- Защита общих ресурсов с помощью mutex
- Обнаружение возможных deadlock (timedlock)
- Логирование в файл log.txt

## Тест

make run
