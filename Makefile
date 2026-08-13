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

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(DEP) $(BIN) palette src/*.o.tmp src/vendor/*.o.tmp

.PHONY: all install clean
