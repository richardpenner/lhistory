VERSION = 0.5.1
CC = cc
CFLAGS = -O2 -Wall -Wextra -pedantic -std=c11 -DLHISTORY_VERSION='"$(VERSION)"'
CFLAGS_SQLITE = -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION
LDFLAGS = -lpthread
PREFIX ?= /usr/local

SRC = src/main.c src/db.c src/tui.c src/input.c src/term.c src/ide.c
OBJ = $(SRC:.c=.o) vendor/sqlite3.o

lhistory: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

vendor/sqlite3.o: vendor/sqlite3.c
	$(CC) $(CFLAGS_SQLITE) -c -o $@ $<

TEST_SRC = tests/test_db.c src/db.c src/ide.c vendor/sqlite3.c
TEST_TUI_SRC = tests/test_tui.c src/tui.c src/term.c src/input.c src/db.c src/ide.c vendor/sqlite3.c

test_db: $(TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_tui: $(TEST_TUI_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: test_db test_tui
	./test_db
	./test_tui

clean:
	rm -f $(OBJ) lhistory test_db test_tui

install: lhistory
	install -d $(PREFIX)/bin $(PREFIX)/share/lhistory
	install -m 755 lhistory $(PREFIX)/bin/
	install -m 644 shell/* $(PREFIX)/share/lhistory/

uninstall:
	$(PREFIX)/bin/lhistory uninstall || true
	rm -f $(PREFIX)/bin/lhistory
	rm -rf $(PREFIX)/share/lhistory

.PHONY: test clean install uninstall
