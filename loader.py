#!/usr/bin/env python3

import sys
import ctypes
import os


def main():
    if len(sys.argv) != 5:
        print(
            f"Usage: {sys.argv[0]} <lib_path> <key> <input_file> <output_file>")
        sys.exit(1)

    lib_path = sys.argv[1]

    try:
        # Ключ - это байт, поэтому он должен быть в диапазоне 0-255
        key = int(sys.argv[2])
        if not (0 <= key <= 255):
            raise ValueError("Key must be an integer between 0 and 255.")
    except ValueError as e:
        print(f"Error: Invalid key. {e}")
        sys.exit(1)

    input_file = sys.argv[3]
    output_file = sys.argv[4]

    if not os.path.exists(lib_path):
        print(f"Error: Library not found at '{lib_path}'")
        sys.exit(1)

    try:
        lib = ctypes.CDLL(lib_path)
    except OSError as e:
        print(f"Error loading library: {e}")
        sys.exit(1)

    # Определение прототипов функций для надежности
    lib.set_key.argtypes = [ctypes.c_char]
    lib.set_key.restype = None

    lib.caesar.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    lib.caesar.restype = None

    try:
        with open(input_file, 'rb') as f:
            file_data = f.read()
    except FileNotFoundError:
        print(f"Error: Input file not found: '{input_file}'")
        sys.exit(1)

    # Создание двух буферов: источник и назначение
    src_buffer = ctypes.create_string_buffer(file_data)
    dst_buffer = ctypes.create_string_buffer(
        file_data)  # Выделяем буфер нужного размера

    # Установка ключа шифрования
    lib.set_key(key)

    # Выполнение XOR-операции: src → dst (отдельные буферы)
    lib.caesar(src_buffer, dst_buffer, len(file_data))

    # Запись результата из dst_buffer в выходной файл
    with open(output_file, 'wb') as f:
        f.write(dst_buffer.raw)

    with open(output_file, 'wb') as f:
        f.write(dst_buffer.raw[:len(file_data)])

    print(
        f"Success: Processed '{input_file}' -> '{output_file}' with key {key}.")


if __name__ == "__main__":
    main()
