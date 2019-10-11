
# Default directories
srcdir     ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
includedir ?= $(realpath .)
libdir     ?= $(realpath .)

# Link and include from the source directory
CFLAGS += "-I$(realpath .)"
CFLAGS += "-I$(includedir)"
CFLAGS += "-L$(libdir)"

# Warnings and errors in cc
CFLAGS += -Wall -Werror
ifdef CLANG
CFLAGS += -Weverything
endif

.PHONY: all
all: libroutines.a libroutines.so

.PHONY: clean
clean:
	rm -rf examples-bin *.a *.so *.o

routines.o: $(srcdir)/routines.c | $(srcdir)/routines.h
	$(CC) $(CFLAGS) -fPIC -c -o $@ $^

libroutines.a: routines.o
	ar -rcD $@ $^

libroutines.so: routines.o
	$(CC) $(CFLAGS) -shared -o $@ $^

# Examples binaries
examples-bin:
	mkdir -p $@

examples-bin/%: $(srcdir)/examples/%.c examples-bin libroutines.a
	$(CC) $(CFLAGS) -o $@ $< -lroutines

.PHONY: examples
examples: $(patsubst examples/%.c,examples-bin/%,$(wildcard examples/*.c))
