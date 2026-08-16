
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

    reads("git status");
    reads("git status --short");
    reads("git log --oneline -5");
    reads("git diff --stat");
    reads("git show HEAD");
    reads("git blame src/ui.c");
    reads("git rev-parse HEAD");
    reads("git -C /tmp/repo status");
    reads("git --no-pager log -3");
    reads("git show HEAD | head -20");

    reads("ls -la | wc -l");
    reads("grep -rn tool src | head -5");
    reads("cat f | sed 's/a/b/' | sort | uniq -c");
    reads("ls src && ls tools");
    reads("cat missing 2>/dev/null | wc -l");
    reads("grep -c . src/ui.c 2>&1");

    acts("rm -rf build");
    acts("make");
    acts("git commit -m wip");
    acts("git checkout main");
    acts("git branch -D old");
    acts("git tag v1");
    acts("git");
    acts("git -c user.name=x commit");
    acts("./mux --help");
    acts("sudo ls");
    acts("ls | xargs rm");

    acts("ls > listing.txt");
    acts("cat a >> b");
    acts("echo hi");
    acts("sed -i 's/a/b/' file");
    acts("cat f | sed 's/a/b/; w out'");
    acts("cat f | awk '{print > \"out\"}'");
    acts("find . -name '*.o' -delete");
    acts("find . -name '*.c' -exec rm {} ;");
    acts("ls $(rm -rf x)");
    acts("ls `rm -rf x`");
    acts("cat <(rm -rf x)");
    acts("ls &");
    acts("wc -l < file");

    acts("sort");
    acts("sed 's/a/b/'");
    acts("awk '{print $1}'");

    expect(toolstyle_collapses("Read", "{\"file_path\":\"/tmp/a\"}", NULL), 1, "Read");
    expect(toolstyle_collapses("web", "{\"query\":\"COLMAP\"}", NULL), 1, "web");
    expect(toolstyle_collapses("web_fetch", "{\"url\":\"https://example.com\"}", NULL), 1,
           "web_fetch");
    expect(toolstyle_collapses("Glob", "{\"pattern\":\"**/*.c\"}", NULL), 1, "Glob");
    expect(toolstyle_collapses("Edit", "{\"file_path\":\"/tmp/a\"}", NULL), 0, "Edit");
    expect(toolstyle_collapses("Write", "{\"file_path\":\"/tmp/a\"}", NULL), 0, "Write");
    expect(toolstyle_collapses("Task", NULL, NULL), 0, "Task");
    expect(toolstyle_collapses("Bash", "{\"command\":\"ls -la\"}", NULL), 1, "Bash(ls)");
    expect(toolstyle_collapses("Bash", "{\"command\":\"make clean\"}", NULL), 0, "Bash(make)");

    expect(toolstyle_collapses("bash", NULL, "ls -la"), 1, "bash arg only");
    expect(toolstyle_collapses("bash", NULL, "rm -rf /"), 0, "bash arg only, acting");
    expect(toolstyle_collapses("bash", NULL, NULL), 0, "bash with nothing to judge");

    acts("cat a\nrm -rf b");

    if (failures == 0)
        printf("toolstyletest: all checks passed\n");
    return failures != 0;
}
