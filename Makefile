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

all: $(LIB_NAME).$(LIB_EXT) secure_copy secure_copy_mt
run: mt_test

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

secure_copy_mt: secure_copy_mt.c
	$(CC) secure_copy_mt.c -o secure_copy_mt $(CFLAGS) -L. -lcaesar

mt_test: all
	@echo "Creating test files..."
	@echo "File1 content" > f1.txt
	@echo "File2 content" > f2.txt
	@echo "File3 content" > f3.txt
	@echo "File4 content" > f4.txt
	@echo "File5 content" > f5.txt

	@echo "Running multithreaded copy..."
	./secure_copy_mt f1.txt f2.txt f3.txt f4.txt f5.txt outdir 42

	@echo "Checking that files were created..."
	@ls outdir

	@echo "Checking log..."
	@cat log.txt
