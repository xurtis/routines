
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
	$(CC) $(CFLAGS) -fPIC -c -o $@ $(filter %.c,$^)

libroutines.a: routines.o
	ar -rcD $@ $^

libroutines.so: routines.o
	$(CC) $(CFLAGS) -shared -o $@ $^

# Examples binaries
examples/%: $(srcdir)/examples/%.c libroutines.a
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) -lroutines

.PHONY: examples
examples: $(patsubst %.c,%,$(wildcard examples/*.c))
