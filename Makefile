# Определяем ОС и настраиваем переменные
UNAME_S := $(shell uname -s)

LIB_PREFIX = lib
LIB_EXT = .so
INSTALL_CMD = sudo cp
# По умолчанию для Linux

# Переопределяем для Windows (в среде MINGW/Git Bash)
ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    LIB_EXT = .dll
    INSTALL_CMD = echo "Install target is not applicable on Windows."
endif

LIB_NAME = $(LIB_PREFIX)caesar$(LIB_EXT)
LOADER = python3 loader.py
CXX = g++
CXXFLAGS = -Wall -Wextra -pedantic -fPIC

.PHONY: all test clean install

# Цель по умолчанию: собрать библиотеку
all: $(LIB_NAME)

$(LIB_NAME): caesar.cpp caesar.h
	$(CXX) $(CXXFLAGS) -shared -o $@ caesar.cpp

test: all
	@echo "--- Starting Test ---"
	@echo "1. Creating test file 'input.txt'..."
	@echo "Hello World from a clean test! 123." > input.txt
	@cat input.txt

	@echo "2. Encrypting 'input.txt' -> 'encrypted.bin'..."
	$(LOADER) ./$(LIB_NAME) 42 input.txt encrypted.bin

	@echo "3. Decrypting 'encrypted.bin' -> 'decrypted.txt'..."
	$(LOADER) ./$(LIB_NAME) 42 encrypted.bin decrypted.txt
	@cat decrypted.txt

	@echo "4. Verifying correctness..."
	@cmp input.txt decrypted.txt && echo "✅ TEST PASSED!" || (echo "Files input.txt and decrypted.txt differ" && echo "❌ TEST FAILED!" && exit 1)


# Копирует библиотеку в системную директорию (только для Linux)
install: all
	$(INSTALL_CMD) $(LIB_NAME) /usr/local/lib/
ifeq ($(findstring MINGW,$(UNAME_S)),)
	sudo ldconfig
	@echo "Installation complete."
endif

# Очистка
clean:
	@echo "Cleaning up generated files..."
	rm -f $(LIB_NAME) input.txt encrypted.bin decrypted.txt
