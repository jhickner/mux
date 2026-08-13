/* Checks which tool calls collapse to a dim row. Pure: the interesting cases
 * are shell commands that must not be mistaken for reads, and those are cheaper
 * to state here than to provoke out of a live model. */
#include <stdio.h>

#include "toolstyle.h"

static int failures;

static void expect(int got, int want, const char *what)
{
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static void reads(const char *command) { expect(toolstyle_shell_reads(command), 1, command); }
static void acts(const char *command) { expect(toolstyle_shell_reads(command), 0, command); }

int main(void)
{
    /* Plain lookups. */
    reads("ls");
    reads("ls -la src");
    reads("pwd");
    reads("cat src/session.c");
    reads("grep -rn tool src");
    reads("rg --files-with-matches cluster");
    reads("find . -name '*.c'");
    reads("stat src/ui.c");
    reads("wc -l src/*.c");
    reads("head -n 20 src/ui.c");
    reads("git grep TODO");
    reads("git ls-files");

    /* Pipelines: a stage rides on the reader that feeds it. */
    reads("ls -la | wc -l");
    reads("grep -rn tool src | head -5");
    reads("cat f | sed 's/a/b/' | sort | uniq -c");
    reads("ls src && ls tools");
    reads("cat missing 2>/dev/null | wc -l");
    reads("grep -c . src/ui.c 2>&1");

    /* Anything that acts, or that cannot be vouched for. */
    acts("rm -rf build");
    acts("make");
    acts("git status");
    acts("git commit -m wip");
    acts("./simple-agent --help");
    acts("sudo ls");
    acts("ls | xargs rm");

    /* Read-only words with an edge that writes. */
    acts("ls > listing.txt");
    acts("cat a >> b");
    acts("echo hi");                 /* content, not a lookup, and no reader */
    acts("sed -i 's/a/b/' file");    /* in place */
    acts("cat f | sed 's/a/b/; w out'");
    acts("cat f | awk '{print > \"out\"}'");
    acts("find . -name '*.o' -delete");
    acts("find . -name '*.c' -exec rm {} ;");
    acts("ls $(rm -rf x)");
    acts("ls `rm -rf x`");
    acts("cat <(rm -rf x)");
    acts("ls &");
    acts("wc -l < file");

    /* A stage with nothing upstream would sit on stdin. */
    acts("sort");
    acts("sed 's/a/b/'");
    acts("awk '{print $1}'");

    /* Tool names, for the backends that report structured input. */
    expect(toolstyle_collapses("Read", "{\"file_path\":\"/tmp/a\"}", NULL), 1, "Read");
    expect(toolstyle_collapses("Glob", "{\"pattern\":\"**/*.c\"}", NULL), 1, "Glob");
    expect(toolstyle_collapses("Edit", "{\"file_path\":\"/tmp/a\"}", NULL), 0, "Edit");
    expect(toolstyle_collapses("Write", "{\"file_path\":\"/tmp/a\"}", NULL), 0, "Write");
    expect(toolstyle_collapses("Task", NULL, NULL), 0, "Task");
    expect(toolstyle_collapses("Bash", "{\"command\":\"ls -la\"}", NULL), 1, "Bash(ls)");
    expect(toolstyle_collapses("Bash", "{\"command\":\"make clean\"}", NULL), 0, "Bash(make)");
    /* The drivers with no structured input hand over the command itself. */
    expect(toolstyle_collapses("bash", NULL, "ls -la"), 1, "bash arg only");
    expect(toolstyle_collapses("bash", NULL, "rm -rf /"), 0, "bash arg only, acting");
    expect(toolstyle_collapses("bash", NULL, NULL), 0, "bash with nothing to judge");

    /* A newline is a separator like any other: the second line is judged too. */
    acts("cat a\nrm -rf b");

    if (failures == 0)
        printf("toolstyletest: all checks passed\n");
    return failures != 0;
}
