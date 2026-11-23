#define _GNU_SOURCE
#include "misto_lipc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int misto_tool_int(const char *tool, const char *service, const char *property, const int *value, int *out)
{
    char number[16];
    if (value) snprintf(number, sizeof number, "%d", *value);
    int result[2];
    if (pipe2(result, O_CLOEXEC)) return -1;
    struct reply { int status; char text[128]; } reply = { .status = -1 };
    pid_t pid = fork();
    if (!pid) {
        close(result[0]);
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() == 1) _exit(127);
        /* hassmic ignores SIGCHLD; native LIPC tools and this supervisor must
         * be allowed to wait for children. The parent gets their real status. */
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        int output[2];
        if (!pipe2(output, O_CLOEXEC)) {
            pid_t child = fork();
            if (!child) {
                prctl(PR_SET_PDEATHSIG, SIGKILL);
                if (getppid() == 1) _exit(127);
                dup2(output[1], STDOUT_FILENO);
                close(output[0]); close(output[1]); close(result[1]);
                if (value) execl(tool, tool, "-i", service, property, number, (char *)0);
                else execl(tool, tool, service, property, (char *)0);
                _exit(127);
            }
            close(output[1]);
            size_t used = 0;
            while (child > 0 && used < sizeof reply.text - 1) {
                ssize_t n = read(output[0], reply.text + used, sizeof reply.text - 1 - used);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                used += (size_t)n;
            }
            close(output[0]);
            if (child > 0) while (waitpid(child, &reply.status, 0) < 0) {
                if (errno != EINTR) { reply.status = -1; break; }
            }
        }
        ssize_t n = write(result[1], &reply, sizeof reply); (void)n;
        close(result[1]);
        _exit(0);
    }
    close(result[1]);
    int ok = -1;
    if (pid > 0) {
        struct pollfd p = { result[0], POLLIN, 0 };
        int r;
        do { r = poll(&p, 1, 1500); } while (r < 0 && errno == EINTR);
        if (r > 0 && read(result[0], &reply, sizeof reply) == sizeof reply &&
            reply.status >= 0 && WIFEXITED(reply.status) && WEXITSTATUS(reply.status) == 0) {
            if (value) ok = 0;
            else {
                char *end; errno = 0;
                long v = strtol(reply.text, &end, 10);
                if (end != reply.text) {
                    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') end++;
                    if (!errno && !*end && v >= INT_MIN && v <= INT_MAX) { if (out) *out = (int)v; ok = 0; }
                }
            }
        } else if (r <= 0) {
            /* The supervisor is still waiting: kill its group, including the tool. */
            kill(-pid, SIGKILL); kill(pid, SIGKILL);
        }
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    close(result[0]);
    if (ok) fprintf(stderr, "misto: LIPC %s %s.%s failed\n", value ? "set" : "get", service, property);
    return ok;
}

int misto_lipc_int(const char *service, const char *property, const int *value, int *out)
{
    const char *tool = getenv(value ? "MISTO_LIPC_SET" : "MISTO_LIPC_GET");
    if (!tool) tool = value ? "/usr/local/bin/lipc-set-prop" : "/usr/local/bin/lipc-get-prop";
    return misto_tool_int(tool, service, property, value, out);
}
