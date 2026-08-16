CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
LDFLAGS ?=
PREFIX  ?= $(HOME)/.local

BIN     := mux
BUILD   := build
# Each tools/*.c builds a binary of the same name under $(BUILD), derived from
# the directory rather than a copy of the list kept by hand.
TOOLS   := $(patsubst tools/%.c,$(BUILD)/%,$(wildcard tools/*.c))
SRC     := $(wildcard src/*.c) $(wildcard src/vendor/*.c)
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)

all: $(BIN)

# forkpty lives in libutil outside the BSDs.
ifneq ($(shell uname -s),Darwin)
LIBS += -lutil
endif

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

# -MMD -MP emits the .d files that keep object files in step with header edits.
# -Isrc/vendor lets the agent drivers find the single shared cJSON.h.
%.o: %.c
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -MMD -MP -c -o $@ $<

-include $(DEP)

# The harnesses under tools/, built only when asked for: the default target is
# the app alone, and `check` builds just the ones that test themselves.
tests: $(TOOLS)

$(BUILD)/palette: tools/palette.c src/vendor/colors.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc/vendor -o $@ tools/palette.c

$(BUILD)/spintest: tools/spintest.c src/status.o src/prompt.o src/files.o src/paste.o src/settings.o src/tty.o src/ui.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/statustest: tools/statustest.c src/status.o src/tty.o src/ui.o src/settings.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ $^

$(BUILD)/reflowtest: tools/reflowtest.c src/ui.o src/settings.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ $^

$(BUILD)/toolstyletest: tools/toolstyletest.c src/toolstyle.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ $^

$(BUILD)/sessionlisttest: tools/sessionlisttest.c src/sessionlist.c src/vendor/cJSON.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/codextest: tools/codextest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/groktest: tools/groktest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/filedifftest: tools/filedifftest.c src/filediff.o src/ui.o src/settings.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/claudetest: tools/claudetest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/pitest: tools/pitest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/agenttabstest: tools/agenttabstest.c src/agenttabs.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/imagetest: tools/imagetest.c src/image.o src/md.o src/ui.o src/settings.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Isrc/vendor -o $@ $^

$(BUILD)/pastetest: tools/pastetest.c src/paste.o | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ $^

$(BUILD)/transcripttest: tools/transcripttest.c src/transcript.c | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ $^

CHECKS  := reflowtest toolstyletest sessionlisttest claudetest codextest \
           groktest filedifftest pitest agenttabstest statustest transcripttest

check: $(addprefix $(BUILD)/,$(CHECKS))
	$(BUILD)/reflowtest
	$(BUILD)/toolstyletest
	$(BUILD)/sessionlisttest
	$(BUILD)/claudetest
	$(BUILD)/codextest
	$(BUILD)/groktest
	$(BUILD)/filedifftest
	$(BUILD)/pitest
	$(BUILD)/agenttabstest
	$(BUILD)/statustest
	$(BUILD)/transcripttest

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
	rm -f $(OBJ) $(DEP) $(BIN) src/*.o.tmp src/vendor/*.o.tmp

.PHONY: all install clean check tests
