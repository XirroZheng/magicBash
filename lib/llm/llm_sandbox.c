#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "llm_config.h"
#include "llm_safety.h"
#include "llm_sandbox.h"

/* A query and its tool calls share a disposable workspace. */
static int enabled;
static char root_path[4096];
static char sandbox_cwd[4096];
static char executable[4096];
static char helper[4096];
static char session[64];

static int run_helper(const char *action, const char *arg1, const char *arg2,
                      const char *arg3, char **capture)
{
    enum { capture_limit = BASH_LLM_MAX_OUTPUT_CAPTURE };
    int fds[2];
    sigset_t block, previous;
    if (capture) {
        *capture = NULL;
        if (pipe(fds) < 0)
            return 1;
    }
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &block, &previous) < 0) {
        if (capture) { close(fds[0]); close(fds[1]); }
        return 1;
    }

    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &previous, NULL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (capture) {
            if (dup2(fds[1], STDOUT_FILENO) < 0)
                _exit(127);
            close(fds[0]);
            close(fds[1]);
        }
        char *const argv[] = {"/usr/bin/python3", "-I", helper, (char *)action,
                              session, (char *)arg1, (char *)arg2, (char *)arg3, NULL};
        char *const env[] = {"PATH=/usr/bin:/bin", "LANG=C.UTF-8", NULL};
        execve(argv[0], argv, env);
        perror("aibash: start sandbox helper");
        _exit(127);
    }

    if (capture) {
        close(fds[1]);
        if (pid > 0) {
            char buffer[4096];
            char *output = malloc(capture_limit + 1);
            size_t used = 0;
            ssize_t n;
            do {
                n = read(fds[0], buffer, sizeof(buffer));
                if (n > 0 && output && used < capture_limit) {
                    size_t available = capture_limit - used;
                    size_t count = (size_t)n < available ? (size_t)n : available;
                    memcpy(output + used, buffer, count);
                    used += count;
                }
            } while (n > 0 || (n < 0 && errno == EINTR));
            if (output) {
                output[used] = '\0';
                *capture = output;
            }
        }
        close(fds[0]);
    }
    int status;
    pid_t waited = -1;
    if (pid > 0)
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    sigprocmask(SIG_SETMASK, &previous, NULL);
    if (waited < 0)
        return 1;
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int llm_sandbox_begin(const char *root, const char *cwd)
{
    enabled = root && *root;
    if (!enabled)
        return 0;

    char *resolved = realpath(root, NULL);
    size_t n = resolved ? strlen(resolved) : 0;
    if (!resolved || n <= 1 || n >= sizeof(root_path) ||
        strncmp(cwd, resolved, n) != 0 || (cwd[n] && cwd[n] != '/')) {
        fprintf(stderr, "aibash: set sandbox_root to a project directory containing the current directory\n");
        free(resolved);
        return 1;
    }
    memcpy(root_path, resolved, n + 1);
    free(resolved);
    if (snprintf(sandbox_cwd, sizeof(sandbox_cwd), "/work%s", cwd + n) >= sizeof(sandbox_cwd))
        return 1;

    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size < 0 || size == sizeof(executable) - 1) {
        fprintf(stderr, "aibash: could not resolve sandbox runtime from the current executable\n");
        return 1;
    }
    executable[size] = '\0';
    const char *slash = strrchr(executable, '/');
    if (!slash)
        return 1;
    int prefix = slash - executable;
    if (snprintf(helper, sizeof(helper), "%.*s/lib/llm/aibash-sandbox.py",
                 prefix, executable) < sizeof(helper) && access(helper, R_OK) == 0)
        return 0;
    if (snprintf(helper, sizeof(helper), "%.*s/aibash-sandbox", prefix, executable) < sizeof(helper)
        && access(helper, R_OK) == 0)
        return 0;
    fprintf(stderr, "aibash: sandbox helper is missing\n");
    return 1;
}

int llm_sandbox_enabled(void)
{
    return enabled;
}

const char *llm_sandbox_cwd(void)
{
    return enabled ? sandbox_cwd : NULL;
}

char *llm_sandbox_execute(const char *name, const char *args, int *exit_status)
{
    if (!session[0]) {
        strcpy(session, "/tmp/aibash-sandbox.XXXXXX");
        if (!mkdtemp(session)) {
            session[0] = '\0';
            return strdup("error: could not create sandbox workspace");
        }
        if (run_helper("init", root_path, sandbox_cwd, executable, NULL) != 0) {
            run_helper("clean", NULL, NULL, NULL, NULL);
            session[0] = '\0';
            return strdup("error: could not prepare Bubblewrap workspace");
        }
    }
    char *output = NULL;
    int status = run_helper("tool", name, args, exit_status ? "direct" : "capture",
                            exit_status ? NULL : &output);
    if (exit_status) {
        *exit_status = status;
        return NULL;
    }
    if (status != 0) {
        free(output);
        return strdup("error: sandbox tool failed");
    }
    if (strcmp(name, "cd") == 0 && output && output[0] == '/' &&
        strlen(output) < sizeof(sandbox_cwd))
        strcpy(sandbox_cwd, output);
    return output ? output : strdup("");
}

int llm_sandbox_finish(int allow_apply)
{
    int status = 0;
    if (session[0]) {
        if (allow_apply) {
            char *preview = NULL;
            status = run_helper("preview", NULL, NULL, NULL, &preview);
            if (status == 0 && preview && preview[0]) {
                fputs(preview, stderr);
                fputc('\n', stderr);
                if (llm_safety_confirm("Apply these sandbox changes to the project?"))
                    status = run_helper("apply", NULL, NULL, NULL, NULL);
                else
                    fprintf(stderr, "aibash: sandbox changes discarded\n");
            }
            free(preview);
        }
        if (run_helper("clean", NULL, NULL, NULL, NULL) && status == 0)
            status = 1;
        session[0] = '\0';
    }
    enabled = 0;
    return status;
}
