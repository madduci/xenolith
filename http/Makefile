# Xenolith HTTP server module
CC       ?= cc
AR       ?= ar
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS   += -std=c11 -D_GNU_SOURCE -pthread
CPPFLAGS += -Iinclude -Isrc
LDLIBS   += -lmicrohttpd -ljansson -lpthread

PREFIX   ?= /usr/local
LIBNAME   = libxenolith_server.a

SRC = src/xenolith_server.c \
      src/openai_api.c \
      src/anthropic_api.c \
      src/xenolith_backend.c \
      src/sse_stream.c

OBJ = $(SRC:.c=.o)

.PHONY: all clean install example

all: $(LIBNAME)

$(LIBNAME): $(OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

example: examples/xenolith_serve.c $(LIBNAME)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIBNAME) $(LDLIBS) -o $@

clean:
	rm -f $(OBJ) $(LIBNAME) example

install: $(LIBNAME)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIBNAME) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 include/xenolith_server.h $(DESTDIR)$(PREFIX)/include/