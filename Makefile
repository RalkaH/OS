UNAME_S := $(shell uname -s)

CC = gcc
CXX = g++

CFLAGS = -Wall -pthread
CXXFLAGS = -Wall -fPIC

LIB_NAME = libcaesar

ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    LIB_EXT = dll
else
    LIB_EXT = so
endif

all: $(LIB_NAME).$(LIB_EXT) secure_copy

run: test

$(LIB_NAME).$(LIB_EXT): caesar.cpp
	$(CXX) $(CXXFLAGS) -shared caesar.cpp -o $(LIB_NAME).$(LIB_EXT)

secure_copy: secure_copy.c $(LIB_NAME).$(LIB_EXT)
	$(CC) secure_copy.c -o secure_copy $(CFLAGS) -L. -lcaesar

test: secure_copy
	@echo "Creating test files..."
	@echo "File1 content" > f1.txt
	@echo "File2 content" > f2.txt
	@echo "File3 content" > f3.txt
	@echo "File4 content" > f4.txt
	@echo "File5 content" > f5.txt

	@echo "Removing old output directory..."
	@rm -rf outdir

	@echo "Running secure_copy..."
	./secure_copy f1.txt f2.txt f3.txt f4.txt f5.txt outdir 42

	@echo "Checking created files..."
	@ls outdir

	@echo "Checking log..."
	@cat log.txt

wildtest: secure_copy
	@echo "Creating wildcard test files..."
	@rm -rf testfiles outdir_wild
	@mkdir -p testfiles
	@echo "Alpha" > testfiles/f1.txt
	@echo "Beta" > testfiles/f2.txt
	@echo "Gamma" > testfiles/f3.txt
	@echo "Delta" > testfiles/f4.txt
	@echo "Epsilon" > testfiles/f5.txt

	@echo "Running wildcard test..."
	./secure_copy testfiles/* outdir_wild 7

	@echo "Checking created files..."
	@ls outdir_wild

	@echo "Checking log..."
	@cat log.txt

bigtest: secure_copy
	@echo "Creating many test files..."
	@rm -rf bigtest_files bigout
	@mkdir -p bigtest_files
	@for i in $$(seq 1 20); do echo "File $$i content for task 3" > bigtest_files/file_$$i.txt; done

	@echo "Running secure_copy on many files..."
	./secure_copy bigtest_files/* bigout 9

	@echo "Checking created files..."
	@ls bigout

	@echo "Checking log..."
	@cat log.txt

teachertest: secure_copy
	@echo "Running teacher-style test in current directory..."
	@rm -rf /tmp/out /tmp/out2
	./secure_copy * /tmp/out 1
	@echo "Contents of /tmp/out:"
	@ls /tmp/out
	./secure_copy /tmp/out/* /tmp/out2 1
	@echo "Contents of /tmp:"
	@ls /tmp

showlog:
	@cat log.txt

clean:
	rm -f secure_copy secure_copy.exe $(LIB_NAME).so $(LIB_NAME).dll
	rm -f *.bin *.txt log.txt
	rm -rf outdir outdir_wild bigout testfiles bigtest_files
	rm -rf /tmp/out /tmp/out2