# simple-agent

A REPL interface to a headless coding-agent CLI, driven through `backend.h`:
`claude` by default, or `codex`, `grok` and `pi` with `-b`. Not a TUI: no
alternate screen, no full-screen redraw. The conversation is ordinary terminal
scrollback, and only the input block is repainted in place.

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
- Four backends behind one interface: `-b claude` (default), `codex`, `grok`,
  `pi`. Each keeps one process alive across turns; what a backend reports it
  reports, and the display drops what it does not (see Backends)
- Your skills, CLAUDE.md, plugins, hooks, MCP servers and custom agents load by
  default; any slash command it does not own is passed to the CLI, so `/w`,
  `/todo` and the rest work. `-s` runs without them
- Authenticates with the claude.ai subscription (`ANTHROPIC_API_KEY` is unset in
  the child); `/session` reports which credential is in use
- Live rendering: assistant text, tool calls, and tool results appear as they
  stream, with a spinner and elapsed time. Reasoning shows as dimmed ✻ rows,
  which `/thinking` hides for good
- A call that only reads — a file read, or a shell command built entirely of
  read-only parts, including the reporting halves of `git` — collapses to one
  dimmed row with no output preview, and a run of them to the same tool is
  listed on a single row. Anything that acts keeps its full block
- `/tools compact` gives every call that one-row treatment, whatever it does
- What the agent says starts at column 0; everything it does is indented, so a
  reply is never mistaken for a tool block
- Per-turn footer: elapsed time, context used against the model's window, and
  cumulative cost
- Markdown rendering: headings, bullets, ordered lists, fenced code, quotes,
  rules, inline code, bold, italic, links
- Multi-line input, word-wrapped and reflowed on terminal resize
- Command completion dropdown, history with reverse search, emacs editing keys
- `@` file completion: fuzzy-matched paths from the working directory (git's
  file list where there is one, so ignored files stay out)
- Type while a turn runs: the prompt stays live under the spinner, and each
  message submitted there is queued and run, in order, once the turn ends
- Interrupt an in-flight turn without losing the session
- Model switching mid-conversation, carrying context across the restart
- Resume any past conversation for the current directory
- Fork a conversation into a second agent: a git worktree off HEAD carrying your
  uncommitted changes, opened in a tmux split or window, resumed on the same
  context. The fork commands run immediately even with a turn in flight, so a
  second agent can be started while the first is still working
- Quitting prints the command that reopens the conversation you were in
- One-shot mode: `simple-agent "question"` prints the answer and exits; piped
  output is plain text with no spinner or footer
- Reports its turn status to
  [tmux-agent-tabs](https://github.com/jhickner/tmux-agent-tabs) when that
  plugin is installed, so the tmux tab spins while a turn runs and marks a
  background one that finished — including a turn you interrupted, which the
  CLI's own hook never reports

## Commands

| Command | Effect |
|---|---|
| `/new`, `/clear` | Start a fresh conversation |
| `/model [name]` | Switch model; no argument opens a picker |
| `/effort [level]` | Set reasoning/thinking effort; no argument opens a backend-specific picker |
| `/thinking [on\|off]` | Show or hide the ✻ reasoning rows; no argument flips it |
| `/tools [compact\|full]` | One row per tool call, or full blocks; no argument flips it |
| `/resume` | Pick a past conversation for this directory |
| `/fh`, `/fs` | Fork into a horizontal tmux split, in a new worktree |
| `/fv` | Fork into a vertical tmux split, in a new worktree |
| `/fw` | Fork into a tmux window, in a new worktree |
| `/session` | Backend, model, auth, session id, cwd, turns, context, cost |
| `/copy` | Copy the last response to the clipboard |
| `/help` | Command and key reference |
| `/quit` | Leave |

Any other `/command` is forwarded to the agent CLI, which is how skills are
invoked (`/todo buy milk`, `/w what do I know about X`).

## Backends

| | claude | codex | grok | pi |
|---|---|---|---|---|
| Streamed text and reasoning | ✓ | ✓ | ✓ | ✓ |
| Tool calls and their output | ✓ | ✓ | ✓ | tool names |
| Tokens, context window, cost | ✓ | — | — | — |
| Resume a past conversation | ✓ | ✓ | ✓ | — |
| `/new` without restarting | ✓ | ✓ | restarts | ✓ |
| `/model` picker | ✓ | pass a name | pass a name | pass a name |
| `/effort` | ✓ | ✓ | ✓ | ✓ |

`/resume` and `-r` list this directory's transcripts: Claude Code's under
`~/.claude/projects`, Grok's under `~/.grok/sessions`. `--session` and the
command printed on quit work wherever resume is. Forking needs a session id and
is available wherever resume is. Pi has no default model configured by itself
— pass one with `-m`.

Codex command executions retain their structured command and working directory;
file changes appear as `Edit` calls with the app-server's complete multi-file
diff.

## Keys

| Key | Action |
|---|---|
| `enter` | Submit, or queue the message when a turn is running |
| `ctrl-j` | Insert a newline |
| `tab` | Accept the completion |
| `@` | Open the file picker; keep typing to filter, `tab` or `enter` to take it |
| `up` / `down` | Browse history; with a message queued, `up` recalls it for editing |
| `ctrl-r` | Reverse history search |
| `esc` | Interrupt the model or a running tool |
| `ctrl-c` | Clear the current line, or interrupt while a turn runs |
| `ctrl-d` | Quit on an empty line, else delete forward |
| `ctrl-l` | Clear the screen |
| `ctrl-a` / `ctrl-e` | Start / end of line |
| `ctrl-w` / `ctrl-u` / `ctrl-k` | Kill word / to start / to end |
| `ctrl-y` | Yank |
| `alt-b` / `alt-f` | Word motion |

## Options

```
simple-agent [-b backend] [-m model] [-e effort] [-C dir] [-s] [-r] [prompt...]

  -b name    agent CLI to drive: claude, codex, grok, pi (default: claude)
  -m model   model to run (default: the CLI's own)
  -e effort  reasoning/thinking effort (default: the CLI's own)
  -C dir     working directory for the agent's tools
  -s         safe mode: skip skills, CLAUDE.md, MCP servers, hooks
  -r         --resume: pick a past conversation to continue
  --session id  resume a specific conversation (used by the fork commands)
  -h         this help
```

## Files

- `~/.config/simple-agent/history` — prompt history
- `~/.config/simple-agent/settings` — `key=value` per line; `/thinking`,
  `/tools` and `/permission` write here
- `~/.claude/projects/<encoded cwd>/*.jsonl` — Claude Code's transcripts,
  read by `/resume` on `-b claude`
- `~/.grok/sessions/<encoded cwd>/<id>/` — Grok's sessions, read by `/resume`
  on `-b grok`

## Build

```sh
make            # -> ./simple-agent
make install    # -> $PREFIX/bin (default ~/.local)
```

Requires the CLI of whichever backend you run to be on `PATH`.

## Vendored libraries

`src/vendor/` holds copies of the single-header libraries, per the convention in
`~/working/libs/c`. `agents/` is a copy of `~/working/libs/c/agents` —
`backend.h` and the four drivers behind it — sharing the one `cJSON.c` beside
it. It diverges in one place: `context_tokens` on the driver and backend results
carries the latest primary-model request's context usage, which `claude.h`
captures from assistant events and `codex.h` from thread token-usage events.
Aggregate usage across every model request in a turn can exceed the context
window many times over, so the footer uses the per-request figure.

`repl.h` diverges in three places:

- a `suggest_off` flag on `Repl` suppresses the inline history autosuggestion;
  `repl_suggestion()` returns NULL when it is set. History browsing and Ctrl-R
  search are unaffected
- `repl_accept_completion()` takes the highlighted candidate from anywhere in
  the line, so Tab completes a token mid-line instead of only at the end (Right
  accepts only at end-of-line, where it cannot be confused with cursor motion).
  Accepting closes the dropdown unless the candidate is a directory, which keeps
  it open to narrow the next path segment. A leading `@` is a trigger rather than
  part of the path, so it survives the replacement and candidates stay bare
- `repl_open_completion()` re-derives the candidates from the token under the
  cursor, so Tab can offer completions for a token the cursor moved back into
  (candidates otherwise only change on an edit)
