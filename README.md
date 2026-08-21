# mux

A coding harness that wraps existing harnesses in headless mode, allowing
subscription use. I use it to test experimental harness features.

- wraps claude, codex, grok, and pi
- switch between backends mid-conversation, preserving context
- switch between sessions with different backends, in the same mux instance
- kitty image support, useful for iterating on graphical projects
