CC ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Wpedantic -Wshadow -g
CPPFLAGS ?= -D_GNU_SOURCE
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer

all: havoc

havoc: main.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -O2 -o $@ $<

debug: havoc-debug

check: havoc-debug
	./tests/smoke.sh ./$<

havoc-debug: main.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SAN) -O0 -o $@ $<

clean:
	rm -f havoc havoc-debug

.PHONY: all clean debug check
