UNAME_S := $(shell uname -s)

CC=gcc
CXX=g++

CFLAGS=-Wall -O2 -pthread -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
CXXFLAGS=-Wall -O2 -fPIC -pthread -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64

LIB_NAME=librc4

ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
        LIB_EXT=dll
else
        LIB_EXT=so
endif

all: $(LIB_NAME).$(LIB_EXT) secure_copy

$(LIB_NAME).$(LIB_EXT): rc4.cpp
	$(CXX) $(CXXFLAGS) -shared rc4.cpp -o $(LIB_NAME).$(LIB_EXT)

secure_copy: secure_copy.c
	$(CC) secure_copy.c -o secure_copy $(CFLAGS) -L. -lrc4 -Wl,-rpath,'$$ORIGIN'

# -----------------------------
# Базовые тесты
# -----------------------------
test-add: all
	@echo "=== Создание образа, вложенность 4 ==="
	mkdir -p in/sub/deep/lvl3/lvl4
	@echo "File 1" > in/1.txt
	@echo "File 2" > in/sub/2.txt
	@echo "Deep file" > in/sub/deep/lvl3/lvl4/deep.txt
	./secure_copy -add -key secret -image disk.img in/
	@echo "OK"

test-list: all
	@echo "=== Список файлов ==="
	./secure_copy -list -image disk.img

test-get: all
	@echo "=== Извлечение файла ==="
	./secure_copy -get -image disk.img -key secret -out extracted.txt sub/2.txt
	@cmp in/sub/2.txt extracted.txt && echo "ТЕСТ ПРОЙДЕН: извлечение корректно"

test-deep: all
	@echo "=== Глубокое извлечение ==="
	./secure_copy -get -image disk.img -key secret -out deep_out.txt sub/deep/lvl3/lvl4/deep.txt
	@cmp in/sub/deep/lvl3/lvl4/deep.txt deep_out.txt && echo "ТЕСТ ПРОЙДЕН: глубокое извлечение"

# -----------------------------
# Расширенные тесты
# -----------------------------
test-sequential: all
	@echo "=== 1 файл — последовательный режим ==="
	rm -f disk.img
	@echo "only one" > solo.txt
	./secure_copy -add -key secret -image disk.img solo.txt
	./secure_copy -get -image disk.img -key secret -out solo_out.txt solo.txt
	@cmp solo.txt solo_out.txt && echo "ТЕСТ ПРОЙДЕН: 1 файл (последовательно)"

test-parallel: all
	@echo "=== 3+ файлов — параллельный режим ==="
	rm -f disk.img
	@echo "a" > a.txt && echo "b" > b.txt && echo "c" > c.txt
	./secure_copy -add -key secret -image disk.img a.txt b.txt c.txt
	./secure_copy -get -image disk.img -key secret -out a_out.txt a.txt
	./secure_copy -get -image disk.img -key secret -out b_out.txt b.txt
	./secure_copy -get -image disk.img -key secret -out c_out.txt c.txt
	@cmp a.txt a_out.txt && cmp b.txt b_out.txt && cmp c.txt c_out.txt && echo "ТЕСТ ПРОЙДЕН: 3 файла (параллельно)"

test-append: all
	@echo "=== Добавление в существующий образ ==="
	@echo "Extra file" > extra.txt
	./secure_copy -add -key secret -image disk.img extra.txt
	./secure_copy -list -image disk.img
	./secure_copy -get -image disk.img -key secret -out extra_out.txt extra.txt
	@cmp extra.txt extra_out.txt && echo "ТЕСТ ПРОЙДЕН: добавление в существующий образ"

test-wrong-key: all
	@echo "=== Неверный ключ → мусор ==="
	./secure_copy -get -image disk.img -key wrongpass -out garbage.txt a.txt
	@if ! cmp -s a.txt garbage.txt; then echo "ТЕСТ ПРОЙДЕН: неверный ключ даёт мусор"; else echo "ТЕСТ ПРОВАЛЕН: неверный ключ дал оригинал!"; fi

test-binary: all
	@echo "=== Бинарный файл 10KB ==="
	dd if=/dev/urandom of=binfile.dat bs=1024 count=10 2>/dev/null
	./secure_copy -add -key secret -image disk.img binfile.dat
	./secure_copy -get -image disk.img -key secret -out binfile_out.dat binfile.dat
	@cmp binfile.dat binfile_out.dat && echo "ТЕСТ ПРОЙДЕН: бинарный файл"

test-empty: all
	@echo "=== Пустой файл ==="
	@touch empty.txt
	./secure_copy -add -key secret -image disk.img empty.txt
	./secure_copy -get -image disk.img -key secret -out empty_out.txt empty.txt
	@cmp empty.txt empty_out.txt && echo "ТЕСТ ПРОЙДЕН: пустой файл"

test-mixed: all
	@echo "=== Файлы + директории одновременно ==="
	rm -f disk.img
	@echo "standalone" > standalone.txt
	mkdir -p data/sub
	@echo "nested" > data/sub/nested.txt
	./secure_copy -add -key mypass -image disk.img standalone.txt data/
	./secure_copy -list -image disk.img
	./secure_copy -get -image disk.img -key mypass -out mixed_out.txt sub/nested.txt
	@cmp data/sub/nested.txt mixed_out.txt && echo "ТЕСТ ПРОЙДЕН: смешанный режим"

test-compat: all
	@echo "=== Бинарный формат образа (по ТЗ) ==="
	rm -f disk.img
	@echo "test" > simple.txt
	./secure_copy -add -key abc -image disk.img simple.txt
	@echo "Формат: [4B size][4B name_len][16B salt][name][encrypted_data]"
	@echo "Числовые поля в little-endian (LE32)"
	@od -A x -t x1z -v disk.img | head -5
	@echo "RC4 ключ = пароль || соль, один непрерывный KSA+PRGA на файл"

test-duplicates: all
	@echo "=== Дубликаты имён файлов ==="
	rm -f disk.img
	@echo "VERSION 1" > dup.txt
	./secure_copy -add -key secret -image disk.img dup.txt
	@echo "VERSION 2 (UPDATED)" > dup.txt
	./secure_copy -add -key secret -image disk.img dup.txt
	./secure_copy -list -image disk.img
	./secure_copy -get -image disk.img -key secret -out dup_out.txt dup.txt
	@cmp dup.txt dup_out.txt && echo "ТЕСТ ПРОЙДЕН: дубликаты (возвращена последняя версия)" || echo "ТЕСТ ПРОВАЛЕН: возвращена не последняя версия!"

test-cli-positional: all
	@echo "=== CLI: позиционные аргументы для -get ==="
	rm -f disk.img
	@echo "cli test" > cli_test.txt
	./secure_copy -add -key secret -image disk.img cli_test.txt
	./secure_copy -get -image disk.img -key secret cli_out.txt cli_test.txt
	@cmp cli_test.txt cli_out.txt && echo "ТЕСТ ПРОЙДЕН: позиционные аргументы -get" || echo "ТЕСТ ПРОВАЛЕН"

test-cli-double-dash: all
	@echo "=== CLI: двойное тире ==="
	rm -f disk.img
	@echo "ddash" > ddash.txt
	./secure_copy --add --key secret --image disk.img ddash.txt
	./secure_copy --get --image disk.img --key secret --out ddash_out.txt ddash.txt
	@cmp ddash.txt ddash_out.txt && echo "ТЕСТ ПРОЙДЕН: двойное тире" || echo "ТЕСТ ПРОВАЛЕН"

test-hardlinks: all
	@echo "=== Хардлинки (один файл под разными именами) ==="
	rm -f disk.img
	mkdir -p hl_in
	dd if=/dev/urandom of=hl_in/file1.bin bs=1M count=1 2>/dev/null
	ln hl_in/file1.bin hl_in/file2.bin
	ln hl_in/file1.bin hl_in/file3.bin
	./secure_copy -add -key secret -image disk.img hl_in/
	./secure_copy -list -image disk.img
	./secure_copy -get -image disk.img -key secret -out hl_out1.bin file1.bin
	./secure_copy -get -image disk.img -key secret -out hl_out2.bin file2.bin
	@cmp hl_in/file1.bin hl_out1.bin && cmp hl_in/file2.bin hl_out2.bin && echo "ТЕСТ ПРОЙДЕН: хардлинки" || echo "ТЕСТ ПРОВАЛЕН"

test-1gb: all
	@echo "=== Большой бинарный файл 1GB ==="
	rm -f disk.img
	dd if=/dev/urandom of=gigfile.dat bs=1M count=1024 status=progress
	./secure_copy -add -key secret -image disk.img gigfile.dat
	./secure_copy -get -image disk.img -key secret -out gigfile_out.dat gigfile.dat
	@cmp gigfile.dat gigfile_out.dat && echo "ТЕСТ ПРОЙДЕН: файл 1GB" || echo "ТЕСТ ПРОВАЛЕН"

test-all: test-add test-list test-get test-deep test-sequential test-parallel test-append test-wrong-key test-binary test-empty test-mixed test-duplicates test-cli-positional test-cli-double-dash test-hardlinks
	@echo ""
	@echo "========================================="
	@echo "  ВСЕ ТЕСТЫ ПРОЙДЕНЫ"
	@echo "========================================="

clean:
	rm -f secure_copy $(LIB_NAME).so $(LIB_NAME).dll
	rm -f *.bin *.dat *.txt *.tmp *.img *.jpg *.jpeg *.png *.gif *.bmp *.mp4 *.avi *.mkv *.mov *.pdf *.doc *.docx *.zip *.tar *.gz
	rm -f *Zone.Identifier*
	rm -rf in data hl_in testdir
