#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "llm_router.h"
#include "llm_tools.h"
#include "llm_exec.h"
#include "llm_safety.h"
#include "llm_sandbox.h"
#include "llm_streams.h"
#include "llm_manscan.h"
#include "llm_config.h"
#include "cJSON.h"

extern char *sh_single_quote(const char *);

static const server_config_t *g_conf = NULL;
static int sandbox_worker;

void llm_router_init(const server_config_t *conf)
{
    g_conf = conf;
}

/* Commands that are read-only / safe to run without confirmation */
static const char *safe_commands[] = {
    "ls", "dir", "cat", "head", "tail", "less", "more",
    "wc", "grep", "egrep", "fgrep", "rg",
    "find", "locate", "which", "whereis", "whatis", "type",
    "file", "stat", "du", "df", "free", "uptime",
    "date", "cal", "whoami", "id", "groups", "hostname",
    "uname", "arch", "lsb_release", "lscpu", "lsblk", "lspci", "lsusb",
    "pwd", "echo", "printf", "true", "false",
    "env", "printenv", "set",
    "ps", "top", "htop", "pgrep",
    "ip", "ifconfig", "netstat", "ss", "ping", "host", "dig", "nslookup",
    "git", "svn",
    "man", "info", "help", "apropos",
    "diff", "cmp", "comm", "sort", "uniq", "tr", "cut", "paste",
    "awk", "sed",
    "tee", "xargs",
    "md5sum", "sha256sum", "sha1sum",
    "xxd", "od", "hexdump",
    "tree", "realpath", "basename", "dirname",
    "ldd", "nm", "objdump", "readelf", "strings",
    "dpkg", "apt", "rpm",
    "pip", "npm", "cargo", "go", "rustc", "python", "python3", "node",
    "make", "cmake", "gcc", "g++", "clang", "clang++", "cc",
    "java", "javac", "mvn", "gradle",
    "docker", "kubectl",
    "curl", "wget",
    "tar", "gzip", "gunzip", "zcat", "bzip2", "xz", "zip", "unzip",
    NULL
};

static const char *first_cmd_word(const char *cmd)
{
    static char buf[256];
    int i = 0;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    while (*cmd && *cmd != ' ' && *cmd != '\t' && i < 255)
        buf[i++] = *cmd++;
    buf[i] = '\0';
    return buf;
}

static int pipeline_is_safe(const char **cmds, int n,
                             const cJSON *stdout_f)
{
    if (stdout_f && cJSON_IsString(stdout_f))
        return 0;

    /* This fixed invocation lists services without changing their state. */
    if (n == 1 && strcmp(cmds[0],
                         "systemctl list-units --type=service --state=running --no-pager --no-legend") == 0)
        return 1;

    for (int i = 0; i < n; i++) {
        const char *name = first_cmd_word(cmds[i]);
        int found = 0;
        for (int j = 0; safe_commands[j]; j++) {
            if (strcmp(name, safe_commands[j]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/* Run a single native command with a literal path operand. */
static char *run_native_path(const char *program, const char *flags,
                             const char *path, int *exit_status)
{
    char *quoted_path = sh_single_quote(path);
    size_t size = strlen(program) + strlen(flags) + strlen(quoted_path) + sizeof("  -- ");
    char *command = malloc(size);
    if (!command) {
        free(quoted_path);
        return strdup("error: could not allocate command");
    }
    snprintf(command, size, "%s %s -- %s", program, flags, quoted_path);
    free(quoted_path);

    const char *pipeline[] = {command};
    char *result = llm_exec_pipeline(pipeline, 1, NULL, NULL, 0, exit_status);
    free(command);
    return result;
}

static char *run_native_cat(const char *path, int *exit_status)
{
    if (!path || !*path)
        return strdup("error: cat requires a file path");
    return run_native_path("cat", "", strcmp(path, "-") == 0 ? "./-" : path,
                           exit_status);
}

static char *handle_run(cJSON *args, const char *arguments, int *exit_status)
{
    if (!args) return strdup("error: invalid arguments");

    cJSON *pipeline = cJSON_GetObjectItem(args, "pipeline");
    if (!cJSON_IsArray(pipeline))
        return strdup("error: pipeline must be an array of commands");

    int n = cJSON_GetArraySize(pipeline);
    const char **cmds = calloc(n, sizeof(char *));
    if (!cmds && n > 0) return strdup("error: malloc failed");
    for (int i = 0; i < n; i++) {
        cJSON *cmd = cJSON_GetArrayItem(pipeline, i);
        const char *name = cJSON_IsString(cmd) ? first_cmd_word(cmd->valuestring) : "";
        if (!*name) {
            free(cmds);
            return strdup("error: pipeline commands must be nonempty strings");
        }
        if (*name == '-') {
            free(cmds);
            return strdup("error: each pipeline entry must contain a command with its arguments, not a separate option");
        }
        cmds[i] = cmd->valuestring;
    }

    char desc[2048] = "Execute: ";
    for (int i = 0; i < n; i++) {
        if (i > 0) strncat(desc, " | ", sizeof(desc) - strlen(desc) - 1);
        strncat(desc, cmds[i], sizeof(desc) - strlen(desc) - 1);
    }

    cJSON *stdin_f  = cJSON_GetObjectItem(args, "stdin_file");
    cJSON *stdout_f = cJSON_GetObjectItem(args, "stdout_file");
    cJSON *append_j = cJSON_GetObjectItem(args, "append");

    /* Models sometimes split "cat ./file" into a two-stage pipeline.
       Only reinterpret a literal, non-executable file as cat's operand. */
    struct stat operand;
    if (!llm_sandbox_enabled() && n == 2 && !stdin_f && !stdout_f && strcmp(cmds[0], "cat") == 0
        && strchr(cmds[1], '/') && stat(cmds[1], &operand) == 0
        && S_ISREG(operand.st_mode)
        && !(operand.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        char *result = run_native_cat(cmds[1], exit_status);
        free(cmds);
        return result;
    }

    if (stdin_f && cJSON_IsString(stdin_f)) {
        strncat(desc, " < ", sizeof(desc) - strlen(desc) - 1);
        strncat(desc, stdin_f->valuestring, sizeof(desc) - strlen(desc) - 1);
    }
    if (stdout_f && cJSON_IsString(stdout_f)) {
        int app = append_j && cJSON_IsTrue(append_j);
        strncat(desc, app ? " >> " : " > ", sizeof(desc) - strlen(desc) - 1);
        strncat(desc, stdout_f->valuestring, sizeof(desc) - strlen(desc) - 1);
    }

    if (!sandbox_worker && !pipeline_is_safe(cmds, n, stdout_f)) {
        if (!llm_safety_confirm(desc)) {
            free(cmds);
            return strdup("[denied by user]");
        }
    }

    const char *sin  = (stdin_f && cJSON_IsString(stdin_f)) ? stdin_f->valuestring : NULL;
    const char *sout = (stdout_f && cJSON_IsString(stdout_f)) ? stdout_f->valuestring : NULL;
    int append = (append_j && cJSON_IsTrue(append_j));

    char *result = llm_sandbox_enabled()
        ? llm_sandbox_execute("run", arguments, exit_status)
        : llm_exec_pipeline(cmds, n, sin, sout, append, exit_status);
    if (exit_status) {
        free(cmds);
        return result;
    }

    /* Tier 1: inject whatis summaries for pipeline commands */
    if (g_conf && g_conf->man_enrich >= 1) {
        char *man_ctx = llm_manscan_enrich_pipeline(cmds, n);
        if (man_ctx) {
            stream_man_output(man_ctx);

            size_t mc_len = strlen(man_ctx);
            size_t r_len = result ? strlen(result) : 0;
            char *enriched = malloc(mc_len + r_len + 32);
            snprintf(enriched, mc_len + r_len + 32,
                     "[man context]\n%s[output]\n%s",
                     man_ctx, result ? result : "");
            free(result);
            free(man_ctx);
            result = enriched;
        }
    }

    free(cmds);
    return result ? result : strdup("(no output)");
}

static char *run_native_ls(const cJSON *args, int *exit_status)
{
    const cJSON *flags_arg = cJSON_GetObjectItem(args, "flags");
    const char *flags = flags_arg ? cJSON_GetStringValue(flags_arg) : "";
    if (!flags || (*flags && (flags[0] != '-' || !flags[1] ||
                              flags[1 + strspn(flags + 1, "aAlhs")] != '\0')))
        return strdup("error: ls flags must be one group of -a, -A, -l, -h, or -s");

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "path"));
    return run_native_path("ls", flags, path ? path : ".", exit_status);
}

int llm_router_dispatch_direct(const tool_call_t *tc, int *exit_status)
{
    if (!tc->name || !tc->arguments)
        return 0;

    int is_run = strcmp(tc->name, "run") == 0;
    int is_ls = strcmp(tc->name, "ls") == 0;
    int is_cat = strcmp(tc->name, "cat") == 0;
    if (!is_run && !is_ls && !is_cat)
        return 0;

    cJSON *args = cJSON_Parse(tc->arguments);
    if (!cJSON_IsTrue(cJSON_GetObjectItem(args, "direct_output"))) {
        cJSON_Delete(args);
        return 0;
    }

    *exit_status = 1;
    if (!is_run && llm_sandbox_enabled()) {
        char *error = llm_sandbox_execute(tc->name, tc->arguments, exit_status);
        if (error) {
            fprintf(stderr, "aibash: %s\n", error);
            free(error);
        }
    } else if (is_ls && cJSON_IsString(cJSON_GetObjectItem(args, "prefix"))) {
        char *result = llm_list_directory(args, exit_status);
        if (!result) {
            *exit_status = 1;
            fprintf(stderr, "bash-llm: could not allocate directory listing\n");
        } else if (*exit_status) {
            fprintf(stderr, "bash-llm: %s\n", result);
        } else if (*result) {
            fputs(result, stdout);
        } else {
            const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(args, "empty_message"));
            if (!message || !*message || strpbrk(message, "\r\n"))
                message = "No matches.";
            puts(message);
        }
        fflush(stdout);
        free(result);
    } else {
        char *error;
        if (is_run)
            error = handle_run(args, tc->arguments, exit_status);
        else if (is_cat)
            error = run_native_cat(cJSON_GetStringValue(cJSON_GetObjectItem(args, "path")),
                                   exit_status);
        else
            error = run_native_ls(args, exit_status);
        if (error) {
            fprintf(stderr, "bash-llm: %s\n", error);
            free(error);
        }
    }
    cJSON_Delete(args);
    return 1;
}

char *llm_router_dispatch(const tool_call_t *tc)
{
    if (!tc || !tc->name)
        return strdup("error: invalid tool call");

    stream_tool_call(tc->name, tc->arguments);

    if (strcmp(tc->name, "run") == 0) {
        cJSON *args = cJSON_Parse(tc->arguments ? tc->arguments : "{}");
        char *result = handle_run(args, tc->arguments ? tc->arguments : "{}", NULL);
        cJSON_Delete(args);
        return result;
    }

    const llm_builtin_t *bi = llm_tools_find(tc->name);
    if (!bi)
        return strdup("error: unknown tool");

    if (!sandbox_worker && bi->safety_tier >= SAFETY_CONFIRM) {
        char desc[512];
        snprintf(desc, sizeof(desc), "%s: %s",
                 bi->name, tc->arguments ? tc->arguments : "{}");
        if (!llm_safety_confirm(desc))
            return strdup("[denied by user]");
    }

    const char *arguments = tc->arguments ? tc->arguments : "{}";
    return llm_sandbox_enabled()
        ? llm_sandbox_execute(tc->name, arguments, NULL)
        : bi->handler(arguments);
}

int llm_router_sandbox_worker(char *name, char *arguments, int direct)
{
    tool_call_t call = {0};
    call.name = name;
    call.arguments = arguments;
    sandbox_worker = 1;
    if (direct) {
        int status = 1;
        llm_router_dispatch_direct(&call, &status);
        return status;
    }

    char *result = llm_router_dispatch(&call);
    if (!result)
        return 1;
    fputs(result, stdout);
    free(result);
    return 0;
}
