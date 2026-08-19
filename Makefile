CC      ?= cc
CFLAGS  ?= -O2
LDFLAGS ?=
PREFIX  ?= $(HOME)/.local

# Kept out of CFLAGS so that `make CFLAGS=-g` cannot drop the warning set.
# -Wcast-qual and -Wshadow are worth an occasional manual run but stay out of
# the default set: they only fire on the vendored agent headers and on the
# const cast that execvp's argv signature forces.
WARNINGS := -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes \
            -Wpointer-arith -Wundef -Wformat=2 -Wno-format-nonliteral
ALL_CFLAGS := -std=gnu11 $(WARNINGS) $(CFLAGS) -Isrc -Isrc/vendor

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
LIBS += -pthread
ifneq ($(shell uname -s),Darwin)
LIBS += -lutil
endif

$(BIN): $(OBJ)
	$(CC) $(ALL_CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

# -MMD -MP emits the .d files that keep object files in step with header edits.
# -Isrc/vendor lets the agent drivers find the single shared cJSON.h.
%.o: %.c
	$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)
-include $(wildcard $(BUILD)/*.d)

# The harnesses under tools/, built only when asked for: the default target is
# the app alone, and `check` builds just the ones that test themselves.
tests: $(TOOLS)

$(BUILD)/palette: tools/palette.c src/vendor/colors.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ tools/palette.c

$(BUILD)/spintest: tools/spintest.c src/status.o src/block.o src/prompt.o src/files.o src/paste.o src/settings.o src/tty.o src/ui.o src/viewport.o src/bash.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^) $(LIBS)

$(BUILD)/statustest: tools/statustest.c src/status.o src/block.o src/tty.o src/ui.o src/viewport.o src/settings.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/viewporttest: tools/viewporttest.c src/viewport.o src/ui.o src/tty.o src/settings.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/reflowtest: tools/reflowtest.c src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/toolstyletest: tools/toolstyletest.c src/toolstyle.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/sessionlisttest: tools/sessionlisttest.c src/sessionlist.o src/vendor/cJSON.o src/text.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/codextest: tools/codextest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/groktest: tools/groktest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/filedifftest: tools/filedifftest.c src/filediff.o src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/claudetest: tools/claudetest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/pitest: tools/pitest.c src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/agenttabstest: tools/agenttabstest.c src/agenttabs.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/imagetest: tools/imagetest.c src/image.o src/md.o src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/vendor/impl.o src/vendor/cJSON.o src/text.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/pastetest: tools/pastetest.c src/paste.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/transcripttest: tools/transcripttest.c src/transcript.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/sessionviewtest: tools/sessionviewtest.c src/sessionview.o src/highlight.o src/toolstyle.o src/ui.o src/viewport.o src/block.o src/settings.o src/tty.o src/text.o src/vendor/impl.o src/vendor/cJSON.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

$(BUILD)/highlighttest: tools/highlighttest.c src/highlight.o | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -o $@ $(filter %.c %.o,$^)

CHECKS  := viewporttest reflowtest toolstyletest sessionlisttest claudetest codextest \
           groktest filedifftest pitest agenttabstest statustest transcripttest \
           sessionviewtest highlighttest

check: $(addprefix $(BUILD)/,$(CHECKS))
	@for t in $^; do echo "$$t"; ./$$t || exit 1; done

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)
	@pkill -URG -x $(BIN) 2>/dev/null || true

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
	rm -f $(OBJ) $(DEP) $(BIN) src/*.o.tmp src/vendor/*.o.tmp

.PHONY: all install clean check tests
