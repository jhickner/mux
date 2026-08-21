/*
 * whisper.h — tiny single-header wrapper around whisper.cpp for speech-to-text.
 *
 * Converts an audio file to 16 kHz mono WAV with ffmpeg, then runs the
 * whisper.cpp CLI (`whisper-cli`) to transcribe it. No shell is invoked
 * (fork/exec directly), so arbitrary file paths — spaces, quotes — are safe.
 *
 * Usage (define the implementation in exactly one .c file):
 *
 *     #define WHISPER_IMPLEMENTATION
 *     #include "whisper.h"
 *
 *     char *text = whisper_transcribe("/tmp/note.ogg", NULL);
 *     if (text) { puts(text); free(text); }
 *
 * Requirements (external binaries, found on PATH by default):
 *   - ffmpeg          (any build that decodes your input + writes WAV)
 *   - whisper-cli     `brew install whisper-cpp`
 *   - a ggml model    e.g. ggml-base.en.bin from
 *                     https://huggingface.co/ggerganov/whisper.cpp
 *
 * The first whisper run on Apple Silicon compiles Metal shaders (several
 * seconds); that result is cached, so subsequent calls are ~sub-second for
 * short clips.
 */
#ifndef WHISPER_H
#define WHISPER_H

typedef struct {
    const char *whisper_bin;  /* default: $WHISPER_BIN   or "whisper-cli"        */
    const char *model_path;   /* default: $WHISPER_MODEL or
                                 ~/.local/share/whisper/ggml-base.en.bin         */
    const char *ffmpeg_bin;   /* default: $FFMPEG_BIN    or "ffmpeg"             */
} whisper_config;

/*
 * Transcribe an audio file (any format ffmpeg decodes: ogg/opus, m4a, mp3,
 * wav, ...) to text. Returns a malloc'd, whitespace-trimmed transcript that the
 * caller frees, or NULL on failure (download/convert/model error, or empty
 * result). `cfg` may be NULL for all defaults; unset fields fall back
 * individually. The input file is left in place; a sibling "<path>.16k.wav"
 * scratch file is created and removed.
 */
char *whisper_transcribe(const char *audio_path, const whisper_config *cfg);

#endif /* WHISPER_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef WHISPER_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

/* Resolve a config value: explicit field, else env var, else built-in default. */
static const char *wh_pick(const char *field, const char *env, const char *def) {
    if (field && *field) return field;
    if (env) { const char *e = getenv(env); if (e && *e) return e; }
    return def;
}

/* fork/exec argv[0] with argv; stdout -> out_fd (or /dev/null if -1), stderr ->
 * /dev/null. Returns the child pid, or -1 on fork failure. */
static pid_t wh_spawn(char *const argv[], int out_fd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (out_fd >= 0) dup2(out_fd, STDOUT_FILENO);
        else if (dn >= 0) dup2(dn, STDOUT_FILENO);
        if (dn >= 0) dup2(dn, STDERR_FILENO);
        if (dn >= 0 && dn > STDERR_FILENO) close(dn);
        execvp(argv[0], argv);
        _exit(127);                 /* exec failed (binary not on PATH) */
    }
    return pid;
}

static int wh_wait_ok(pid_t pid) {
    int st;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* Convert `in` to 16 kHz mono WAV at `out`. Returns 0 on success. */
static int wh_to_wav(const char *ffmpeg, const char *in, const char *out) {
    char *argv[] = { (char *)ffmpeg, "-y", "-i", (char *)in,
                     "-ar", "16000", "-ac", "1", "-f", "wav", (char *)out, NULL };
    return wh_wait_ok(wh_spawn(argv, -1)) ? 0 : -1;
}

/* Run whisper-cli on `wav`, capturing its transcript from stdout. Malloc'd or
 * NULL. `-nt` drops timestamps, `-np` drops progress/system chatter. */
static char *wh_run(const char *bin, const char *model, const char *wav) {
    int fd[2];
    if (pipe(fd) != 0) return NULL;
    char *argv[] = { (char *)bin, "-m", (char *)model, "-f", (char *)wav,
                     "-nt", "-np", NULL };
    pid_t pid = wh_spawn(argv, fd[1]);
    close(fd[1]);
    if (pid < 0) { close(fd[0]); return NULL; }

    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (!out) { close(fd[0]); waitpid(pid, NULL, 0); return NULL; }
    char buf[4096];
    ssize_t r;
    while ((r = read(fd[0], buf, sizeof buf)) > 0) {
        if (len + (size_t)r + 1 > cap) {
            cap = (len + (size_t)r + 1) * 2;
            char *np = realloc(out, cap);
            if (!np) { free(out); close(fd[0]); waitpid(pid, NULL, 0); return NULL; }
            out = np;
        }
        memcpy(out + len, buf, (size_t)r);
        len += (size_t)r;
    }
    close(fd[0]);
    out[len] = '\0';
    if (!wh_wait_ok(pid)) { free(out); return NULL; }
    return out;
}

char *whisper_transcribe(const char *audio_path, const whisper_config *cfg) {
    if (!audio_path || !*audio_path) return NULL;

    const char *ffmpeg = wh_pick(cfg ? cfg->ffmpeg_bin : NULL,  "FFMPEG_BIN",  "ffmpeg");
    const char *bin    = wh_pick(cfg ? cfg->whisper_bin : NULL, "WHISPER_BIN", "whisper-cli");

    char model_def[1024];
    const char *model = cfg ? cfg->model_path : NULL;
    if (!model || !*model) model = getenv("WHISPER_MODEL");
    if (!model || !*model) {
        const char *home = getenv("HOME");
        snprintf(model_def, sizeof model_def,
                 "%s/.local/share/whisper/ggml-base.en.bin", home ? home : ".");
        model = model_def;
    }

    /* Scratch WAV alongside the input. */
    size_t wlen = strlen(audio_path) + 9;
    char *wav = malloc(wlen);
    if (!wav) return NULL;
    snprintf(wav, wlen, "%s.16k.wav", audio_path);

    char *text = NULL;
    if (wh_to_wav(ffmpeg, audio_path, wav) == 0)
        text = wh_run(bin, model, wav);
    remove(wav);
    free(wav);
    if (!text) return NULL;

    /* Trim surrounding whitespace/newlines (whisper prints a leading space). */
    char *s = text;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
    if (n == 0) { free(text); return NULL; }
    char *trimmed = malloc(n + 1);
    if (!trimmed) return text;   /* fall back to untrimmed rather than losing it */
    memcpy(trimmed, s, n + 1);
    free(text);
    return trimmed;
}

#endif /* WHISPER_IMPLEMENTATION */
