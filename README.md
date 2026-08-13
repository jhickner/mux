# simple-agent

A REPL interface to the Claude Code CLI, driven through `claude.h`. Not a TUI:
no alternate screen, no full-screen redraw. The conversation is ordinary
terminal scrollback, and only the input block is repainted in place.

```
▌ simple-agent › sonnet-5
▌ ctrl-d quit · try /help

▌ list the files here and summarize notes.txt

  [bash] ls
    notes.txt
  [read] notes.txt
    1 alpha
    +3 lines

The directory holds one file, notes.txt:

• alpha
• beta
• gamma

6s · 87.7k / 1.0M (8%) · $0.0583

❯
```

The line being edited wears a `❯` caret; once submitted it is redrawn as a `▌`
block in the history above.

## Features

- Full Claude Code harness: every built-in tool, subagents, context management
- Your skills, CLAUDE.md, plugins, hooks, MCP servers and custom agents load by
  default; any slash command it does not own is passed to the CLI, so `/w`,
  `/todo` and the rest work. `-s` runs without them
- Authenticates with the claude.ai subscription (`ANTHROPIC_API_KEY` is unset in
  the child); `/session` reports which credential is in use
- Live rendering: assistant text, tool calls, and tool results appear as they
  stream, with a spinner and elapsed time
- Per-turn footer: elapsed time, context used against the model's window, and
  cumulative cost
- Markdown rendering: headings, bullets, ordered lists, fenced code, quotes,
  rules, inline code, bold, italic, links
- Multi-line input, word-wrapped and reflowed on terminal resize
- Command completion dropdown, history with reverse search, emacs editing keys
- Interrupt an in-flight turn without losing the session
- Model switching mid-conversation, carrying context across the restart
- Resume any past conversation for the current directory
- One-shot mode: `simple-agent "question"` prints the answer and exits; piped
  output is plain text with no spinner or footer

## Commands

| Command | Effect |
|---|---|
| `/new`, `/clear` | Start a fresh conversation |
| `/model [name]` | Switch model; no argument opens a picker |
| `/resume` | Pick a past conversation for this directory |
| `/session` | Model, auth, session id, cwd, turns, context, cost |
| `/copy` | Copy the last response to the clipboard |
| `/help` | Command and key reference |
| `/quit` | Leave |

Any other `/command` is forwarded to the claude CLI, which is how skills are
invoked (`/todo buy milk`, `/w what do I know about X`).

## Keys

| Key | Action |
|---|---|
| `enter` | Submit |
| `ctrl-j` | Insert a newline |
| `tab` | Accept the completion or inline suggestion |
| `up` / `down` | Browse history |
| `ctrl-r` | Reverse history search |
| `esc` | Interrupt the model or a running tool |
| `ctrl-c` | Clear the current line |
| `ctrl-d` | Quit on an empty line, else delete forward |
| `ctrl-l` | Clear the screen |
| `ctrl-a` / `ctrl-e` | Start / end of line |
| `ctrl-w` / `ctrl-u` / `ctrl-k` | Kill word / to start / to end |
| `ctrl-y` | Yank |
| `alt-b` / `alt-f` | Word motion |

## Options

```
simple-agent [-m model] [-C dir] [-s] [-r] [prompt...]

  -m model   model to run (default: the claude CLI's own)
  -C dir     working directory for the agent's tools
  -s         safe mode: skip skills, CLAUDE.md, MCP servers, hooks
  -r         --resume: pick a past conversation to continue
  -h         this help
```

## Files

- `~/.config/simple-agent/history` — prompt history
- `~/.claude/projects/<encoded cwd>/*.jsonl` — the CLI's own transcripts, read
  by `/resume`

## Build

```sh
make            # -> ./simple-agent
make install    # -> $PREFIX/bin (default ~/.local)
```

Requires the `claude` CLI on `PATH`.

## Vendored libraries

`src/vendor/` holds copies of the single-header libraries, per the convention in
`~/working/libs/c`. `claude.h` diverges from its upstream copy:

- `claude_send_ex()` and `claude_result` expose the result event's duration,
  token usage, context window, cost, and subtype
- the event callback takes a typed `claude_event` (tool name and input JSON are
  separate fields) instead of two loose strings
- `claude_interrupt()` sends the Agent SDK's interrupt control request; the
  abort predicate now uses it and reads on to the result event, so an
  interrupted session stays usable instead of leaving the stream misaligned
- `claude_model()` and `claude_auth_source()` report what the CLI resolved at
  startup
- `allow_customizations` drops `--safe-mode`, so the CLI loads skills, CLAUDE.md,
  plugins, hooks, MCP servers and custom agents (the `{0}` default stays safe)
- the child's stderr is captured into a bounded buffer rather than inherited, so
  a CLI warning cannot corrupt a caller painting its own display;
  `claude_last_error()` reads it back
- `claude_stop()` signals immediately instead of waiting out Node's ~300ms
  unwind, which an interactive caller feels on quit
- the read poll is 80ms so the abort predicate can double as a UI tick
