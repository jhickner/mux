# Many sessions, one window

A mux process used to be one conversation. The window it ran in *was* the
session: to hold two you opened two windows, and a conversation started
somewhere else stayed there. A window now holds any number of sessions,
switches between them with the left arrow, runs their turns at the same time,
and can pull a session out of another window into itself.

## The list

Left arrow on an empty prompt — or `/sessions` — opens the list of what is
open, wherever it is open:

- **this window's tabs**, marked `▸` and `(here)`
- **every other window's sessions**, marked `⇄`; picking one takes it
- **+ new session**, which asks for a backend and a model

Conversations that are not running are not in it — that is what `/resume` is
for. The list is not filtered by directory either: a window anywhere on the
machine is reachable from any other, which is the point of it. Every row leads
with the directory its session runs in, so that is what survives when a row is
cut to fit, then the backend and model. Typing narrows the list. `^n` opens a
new session, `^x` closes the highlighted tab. When the window holds more than one session, a strip above the prompt
shows them by number and name, coloured by status: accent for the one in
front, the spinner colour for one that is working, red for one that errored.

## What runs where

`workspace.c` owns the tabs. Each holds a session, a screen, its own sticky
prompt, and the lines typed at it that are still waiting.

The screen is the thing that makes this cheap. `viewport.c` was already
retained-mode — it keeps the transcript as an item list and only writes to the
terminal in `viewport_paint()` — so a tab that is not in front simply gets its
own item list. `viewport_stash()`/`viewport_adopt()` swap that list, the open
block, and the scroll offset as a unit; `viewport_hold()` drops paints while a
background tab's list is in place, so a session drawing behind the window
cannot draw over it. Switching is a swap and a repaint, which is why a tab
comes back with its tool calls and diffs intact rather than re-derived from the
transcript.

## Turns that keep running

`session_turn_begin()` runs `ask_ex()` on a thread of its own. That thread
draws nothing: `on_event` copies each event into a per-session queue and writes
to a pipe. The main loop watches that pipe alongside everything else, and
`session_turn_pump()` — called with that session's screen in place — draws the
queued events, then does the turn's tail work when the thread finishes. So all
drawing still happens on one thread, in the same order as before; only the
waiting moved.

Two details the backend layer forced:

- `set_abort_check()` takes a bare `int (*)(void)` with no user data, so the
  session running a turn is kept in a `__thread` pointer. On the main thread
  that pointer is NULL and the old, blocking path is used unchanged — which is
  what one-shot prompts and the headless Telegram front end still take.
- `idle_pump()` would steal a turn's stream, so a tab with a turn in flight is
  watched through its queue pipe and pumped through `session_turn_pump()`
  instead.

The spinner belongs to the tab in front; a turn running behind it shows in the
tab strip. Escape or ctrl-C on an empty prompt stops the turn in front. A line
typed while the tab in front is busy waits for it, and is echoed when it is
sent so the transcript keeps the order the agent saw. A command typed behind a
running turn behaves as one typed during a turn always has: it either applies
at once or waits, and `/quit` still leaves.

## What a window shows when a session arrives

A tab this window has held keeps its own screen, so switching back to it is a
swap and a repaint. A session that arrives from somewhere else has no screen
here, and one is built for it:

- **yanked** — the window that had it dumps its screen and sends it along, so
  what appears is exactly what was on the other terminal, tool calls and all.
- **resumed** through `/resume`, or yanked from a window that had nothing to
  send — `sessionload.c`
  reads the CLI's own transcript and draws the conversation back: what was
  asked, what was answered, the reasoning when reasoning is shown, and a line
  per tool call. Tool output is left out; the call above it says what ran. A
  long conversation comes back from the last few hundred turns, with a note
  saying how many are above.
- **started with `--session`** — the same, so a `/fw` fork or a resume from the
  command line opens with the conversation already on screen.

claude, grok, and pi each keep a transcript, and all three write one JSON
object per line with a role and content blocks, so one reader handles them;
codex keeps none, and its sessions open bare.

The window follows the tab in front the way `/cd` makes it follow the session:
it changes directory, re-homes file completion, and re-reads the git
information. A `!` command run after switching runs where that session runs.

## Taking a session from another window

Every interactive mux publishes one record per session under
`~/.config/mux/live/<pid>-<slot>.json`: backend, model, effort, cwd, session
id, name, status, and the tmux pane it is in. Records are written when a
session starts, when a turn changes its status, and when it names itself; they
go when the process exits, and a reader prunes any whose pid is gone.

Picking a `⇄` row runs the handover:

1. The asker writes `~/.config/mux/handoff/<pid>.req` naming the session it
   wants, and signals the other window.
2. The signal is the one a restart already travels on. The window tells the two
   apart by whether a request is waiting for it, and answers between
   keystrokes rather than waiting for a quiet prompt — the asker is blocked on
   it. It dumps that tab's screen, closes the tab (which closes the agent), and
   only then publishes `<id>.state`.
3. The asker takes that as the moment it may open the same conversation, starts
   it from the session id the way `/restart` does, and restores the carried
   screen so the transcript reads as one thing rather than beginning at a bare
   resume.

A window that gives up its last session leaves: there is nothing left for it to
show. A window with other sessions keeps going, one tab lighter, and says so.

Nothing may stand in the way of an answer, because the window that asked is
blocked on it. A list, a picker, or a confirmation open at the time is
cancelled rather than waited on, and a headless bridge with no terminal at all
notices on its own poll and lets go the same way. What it cannot interrupt is a
`!` command or an editor: the window is inside a child then, and the asker
waits it out.

The agent process is never transferred — it is closed on one side and restarted from the session id on the
other — so a backend that cannot resume cannot be yanked, and the window
asking waits at most fifteen seconds before saying so.

## Known limits

- A restart (`/restart`, or `make install` signalling every mux) carries only
  the session in front. The others are closed; their conversations are still
  on disk, so they come back from the `↺` rows of the list.
- The Telegram bridge stays attached to the session it was started with, and
  its lines run on the main thread — so a chat line waits for a turn already
  running at that session before it starts.
- While the list itself is open, nothing is pumped: background turns keep
  running but their output is drawn when the list closes.
- A window sitting at a `!` command or in an editor cannot answer a request for
  one of its sessions until that finishes.

## Testing

`tools/ptydrive.py` drives mux in a pty from a small script of waits and
keystrokes, so the interactive paths — tabs, the switcher, a turn running
behind the prompt, a yank between two processes — can be exercised without
taking over a terminal someone is using. It points `MUX_LIVE_DIR` at a
registry of its own, so a driven mux only ever sees sessions the test started
— `MUX_LIVE_DIR` overrides where the records are kept, the way
`AGENT_TABS_STATE_DIR` already does for the tmux tab records.
`tools/viewporttest.c` covers the screen swap directly.
