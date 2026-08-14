CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
LDFLAGS ?=
PREFIX  ?= $(HOME)/.local

BIN     := simple-agent
SRC     := $(wildcard src/*.c) $(wildcard src/vendor/*.c)
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

# -MMD -MP emits the .d files that keep object files in step with header edits.
# -Isrc/vendor lets the agent drivers find the single shared cJSON.h.
%.o: %.c
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -MMD -MP -c -o $@ $<

-include $(DEP)

palette: tools/palette.c src/vendor/colors.h
	$(CC) $(CFLAGS) -Isrc/vendor -o $@ tools/palette.c

spintest: tools/spintest.c src/status.o src/prompt.o src/files.o src/paste.o src/settings.o src/tty.o src/ui.o src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

statustest: tools/statustest.c src/status.o src/tty.o src/ui.o src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -o $@ $^

reflowtest: tools/reflowtest.c src/ui.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -o $@ $^

toolstyletest: tools/toolstyletest.c src/toolstyle.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -o $@ $^

sessionlisttest: tools/sessionlisttest.c src/sessionlist.c src/vendor/cJSON.c
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

codextest: tools/codextest.c src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

groktest: tools/groktest.c src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

filedifftest: tools/filedifftest.c src/filediff.o src/ui.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

claudetest: tools/claudetest.c src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

pitest: tools/pitest.c src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

agenttabstest: tools/agenttabstest.c src/agenttabs.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

imagetest: tools/imagetest.c src/image.o src/md.o src/ui.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

pastetest: tools/pastetest.c src/paste.o
	$(CC) $(CFLAGS) -Isrc -o $@ $^

check: reflowtest toolstyletest sessionlisttest claudetest codextest groktest filedifftest pitest agenttabstest statustest
	./reflowtest
	./toolstyletest
	./sessionlisttest
	./claudetest
	./codextest
	./groktest
	./filedifftest
	./pitest
	./agenttabstest
	./statustest

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(DEP) $(BIN) palette spintest statustest reflowtest toolstyletest sessionlisttest claudetest codextest groktest filedifftest pitest agenttabstest imagetest pastetest src/*.o.tmp src/vendor/*.o.tmp

.PHONY: all install clean check
