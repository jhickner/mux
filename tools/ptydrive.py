#!/usr/bin/env python3
"""Drive mux in a pty: a script of (wait, keys) pairs, then dump what it drew.

Not part of the build. It exists so the interactive paths — tabs, the
switcher, a turn running behind the prompt — can be exercised without taking
over a terminal someone is using.

    ptydrive.py <seconds-per-step> -- ./mux -m model   << script on stdin

Each stdin line is either `wait N` or `send <text>` with \\e, \\r escapes.
"""
import os, pty, select, subprocess, sys, time

def main():
    args = sys.argv[1:]
    cut = args.index("--")
    cmd = args[cut + 1:]
    script = sys.stdin.read().splitlines()

    master, slave = pty.openpty()
    env = dict(os.environ, TERM="xterm-256color", COLUMNS="100", LINES="30")
    env.pop("TMUX", None)
    env.pop("TMUX_PANE", None)
    # A registry of its own: a driven mux must never see, or take, a session
    # belonging to a window someone is using.
    env.setdefault("MUX_LIVE_DIR", "/tmp/mux-livetest")
    p = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave,
                         env=env, preexec_fn=os.setsid)
    os.close(slave)

    out = []
    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.1)
            if not r:
                continue
            try:
                data = os.read(master, 65536)
            except OSError:
                return
            if not data:
                return
            out.append(data)

    for line in script:
        line = line.strip()
        if line.startswith("wait "):
            pump(float(line[5:]))
        elif line.startswith("send "):
            text = line[5:].replace("\\e", "\x1b").replace("\\r", "\r").replace("\\t", "\t")
            os.write(master, text.encode())
            pump(0.4)
    pump(1.0)
    try:
        p.kill()
    except Exception:
        pass
    sys.stdout.write(b"".join(out).decode("utf-8", "replace"))

main()
