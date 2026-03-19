UNAME_S := $(shell uname -s)

CC=gcc
CXX=g++

CFLAGS=-Wall -pthread
CXXFLAGS=-Wall -fPIC

LIB_NAME=libcaesar

ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    LIB_EXT=dll
else
    LIB_EXT=so
endif

all: $(LIB_NAME).$(LIB_EXT) secure_copy

$(LIB_NAME).$(LIB_EXT): caesar.cpp
	$(CXX) $(CXXFLAGS) -shared caesar.cpp -o $(LIB_NAME).$(LIB_EXT)

secure_copy: secure_copy.c
	$(CC) secure_copy.c -o secure_copy $(CFLAGS) -L. -lcaesar


# -----------------------------
# Тесты
# -----------------------------

test: all
	@echo "Creating test file..."
	@echo "Hello Operating Systems!" > input.txt

	@echo "Encrypting..."
	./secure_copy input.txt encrypted.bin 42

	@echo "Decrypting..."
	./secure_copy encrypted.bin decrypted.txt 42

	@echo "Result:"
	@cat decrypted.txt

	@echo "Checking correctness..."
	@cmp input.txt decrypted.txt && echo "TEST PASSED"


# 10мб тест
bigtest: all
	@echo "Creating 10MB file..."
	dd if=/dev/urandom of=test.bin bs=1M count=10

	@echo "Running secure_copy..."
	./secure_copy test.bin enc.bin 55

	@echo "Done"


clean:
	rm -f secure_copy secure_copy.exe $(LIB_NAME).so $(LIB_NAME).dll *.bin *.txt