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
	$(CC) secure_copy.c -o secure_copy $(CFLAGS) -L. -lcaesar -Wl,-rpath,'$$ORIGIN'


# -----------------------------
# Тест старого режима
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


# -----------------------------
# Тест нового sequential режима
# -----------------------------
test-seq: all
	@echo "Creating files..."
	@echo "Alpha" > f1.txt
	@echo "Beta"  > f2.txt
	@echo "Gamma" > f3.txt

	@echo "Running sequential mode..."
	./secure_copy --mode=sequential --key=42 f1.txt f2.txt f3.txt

	@echo "Decrypting first file via old compatibility mode..."
	./secure_copy f1.txt.enc f1_restored.txt 42

	@echo "Checking correctness..."
	@cmp f1.txt f1_restored.txt && echo "SEQUENTIAL TEST PASSED"


# -----------------------------
# Тест нового parallel режима
# -----------------------------
test-par: all
	@echo "Creating files..."
	@echo "One"   > p1.txt
	@echo "Two"   > p2.txt
	@echo "Three" > p3.txt
	@echo "Four"  > p4.txt
	@echo "Five"  > p5.txt
	@echo "Six"   > p6.txt

	@echo "Running parallel mode..."
	./secure_copy --mode=parallel --key=42 p1.txt p2.txt p3.txt p4.txt p5.txt p6.txt

	@echo "Decrypting one file to verify..."
	./secure_copy p4.txt.enc p4_restored.txt 42

	@echo "Checking correctness..."
	@cmp p4.txt p4_restored.txt && echo "PARALLEL TEST PASSED"


# -----------------------------
# Тест auto режима
# -----------------------------
test-auto: all
	@echo "Creating files..."
	@echo "A" > a1.txt
	@echo "B" > a2.txt
	@echo "C" > a3.txt
	@echo "D" > a4.txt
	@echo "E" > a5.txt
	@echo "F" > a6.txt

	@echo "Running auto mode..."
	./secure_copy --mode=auto --key=42 a1.txt a2.txt a3.txt a4.txt a5.txt a6.txt


# -----------------------------
# Демонстрация на 10 файлах
# -----------------------------
demo10: all
	@echo "Creating 10 demo files..."
	@for i in 1 2 3 4 5 6 7 8 9 10; do \
		dd if=/dev/urandom of=file$$i.bin bs=1M count=2 status=none; \
	done

	@echo "Sequential run..."
	./secure_copy --mode=sequential --key=55 file1.bin file2.bin file3.bin file4.bin file5.bin file6.bin file7.bin file8.bin file9.bin file10.bin

	@echo "Parallel run..."
	./secure_copy --mode=parallel --key=55 file1.bin file2.bin file3.bin file4.bin file5.bin file6.bin file7.bin file8.bin file9.bin file10.bin


# 10мб тест старого формата
bigtest: all
	@echo "Creating 10MB file..."
	dd if=/dev/urandom of=test.bin bs=1M count=10

	@echo "Running secure_copy..."
	./secure_copy test.bin enc.bin 55

	@echo "Done"


clean:
	rm -f secure_copy secure_copy.exe $(LIB_NAME).so $(LIB_NAME).dll *.bin *.txt *.enc *.bench.enc