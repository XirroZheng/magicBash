#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>

#include "llm_shell.h"
#include "llm_api.h"
#include "llm_router.h"
#include "llm_history.h"
#include "llm_streams.h"
#include "cJSON.h"

/* Check bash's interrupt_state for Ctrl-C */
extern volatile sig_atomic_t interrupt_state;

/* Helper to update ctx->last_output */
static void set_last_output(llm_shell_ctx_t *ctx, char *output)
{
    free(ctx->last_output);
    ctx->last_output = output;
}

/* ---- Display model answers ---- */

static int starts_unsolicited_trailer(const char *text)
{
    static const char *const offers[] = {
        "需要我", "你需要我", "您需要我", "还需要我", "还要我", "要我", "要不要我",
        "需不需要我", "还需要吗", "需不需要？", "需不需要?", "需要继续吗",
        "如果需要，我可以", "如果你需要，我可以", "如果你愿意，我可以",
        "如果你想，我可以", "有需要的话，我可以",
        "顺带一提", "顺便一提", "顺便说一句", "附带一提", "额外提醒",
        "友情提示", "温馨提示", "By the way", "Side note:",
        "Would you like me to", "Do you want me to", "Shall I ",
        "If you'd like, I can", "If you want, I can", "Let me know if you",
        NULL
    };

    text += strspn(text, " \t");
    if (strncmp(text, "**", 2) == 0)
        text += 2;
    if (strncmp(text, "需要我们", strlen("需要我们")) == 0)
        return 0;
    for (int i = 0; offers[i]; i++) {
        if (strncasecmp(text, offers[i], strlen(offers[i])) == 0)
            return 1;
    }
    return 0;
}

static void trim_unsolicited_trailer(char *text)
{
    char *cut = NULL;
    char fence = '\0';
    size_t fence_width = 0;
    int has_answer = 0;

    for (char *line = text; line && *line; ) {
        char *next = strchr(line, '\n');
        char *end = next ? next : line + strlen(line);
        char *first = line + strspn(line, " \t");
        size_t width = 0;
        if (*first == '`' || *first == '~') {
            while (first[width] == *first)
                width++;
        }
        if (width >= 3) {
            if (!fence) {
                fence = *first;
                fence_width = width;
            } else if (fence == *first && width >= fence_width
                       && first + width + strspn(first + width, " \t\r") == end) {
                fence = '\0';
            }
        } else if (!fence && first < end) {
            if (has_answer && starts_unsolicited_trailer(line)) {
                cut = line;
                break;
            }
            size_t code_span = 0;
            for (char *p = first; p < end; p++) {
                if (*p == '`') {
                    size_t ticks = strspn(p, "`");
                    if (!code_span || code_span == ticks)
                        code_span = code_span ? 0 : ticks;
                    p += ticks - 1;
                    continue;
                }
                if (code_span)
                    continue;
                size_t stop = 0;
                if (strncmp(p, "。", strlen("。")) == 0
                    || strncmp(p, "！", strlen("！")) == 0
                    || strncmp(p, "？", strlen("？")) == 0)
                    stop = strlen("。");
                else if (strchr(".!?", *p) && p + 1 < end && p[1] == ' ')
                    stop = 1;

                if (stop && p + stop < end && starts_unsolicited_trailer(p + stop)) {
                    cut = p + stop;
                    break;
                }
            }
            if (cut)
                break;
        }
        if (first < end)
            has_answer = 1;
        line = next ? next + 1 : NULL;
    }
    if (cut) {
        while (cut > text && strchr(" \t\r\n", cut[-1]))
            cut--;
        *cut = '\0';
    }
}

/* ---- Agentic loop ---- */

static void llm_shell_agentic_loop(llm_shell_ctx_t *ctx, llm_response_t *resp,
                                   llm_stream_cbs *cbs)
{
    char cwd[4096];
    int iterations = 0;

    while (resp && resp->num_tool_calls > 0
           && !interrupt_state && iterations < ctx->max_iterations) {
        iterations++;

        /* Tool-call turns carry the calls, without a user-facing preamble. */
        llm_history_add_assistant_tool_calls(NULL, resp->tool_calls,
                                              resp->num_tool_calls);

        set_last_output(ctx, NULL);

        /* Execute each tool call and record results */
        for (int i = 0; i < resp->num_tool_calls && !interrupt_state; i++) {
            const char *name = resp->tool_calls[i].name;
            if (streams_label_mode && name) {
                char status[512];
                const char *args = resp->tool_calls[i].arguments;
                if (args && args[0]) {
                    cJSON *a = cJSON_Parse(args);
                    const char *hint = NULL;
                    if (a) {
                        cJSON *child = a->child;
                        while (child) {
                            if (cJSON_IsString(child)) { hint = child->valuestring; break; }
                            if (cJSON_IsArray(child)) {
                                cJSON *first = cJSON_GetArrayItem(child, 0);
                                if (first && cJSON_IsString(first)) { hint = first->valuestring; break; }
                            }
                            child = child->next;
                        }
                        snprintf(status, sizeof(status), "  \xe2\x86\x92 %s %s",
                                 name, hint ? hint : "");
                        cJSON_Delete(a);
                    } else {
                        snprintf(status, sizeof(status), "  \xe2\x86\x92 %s", name);
                    }
                } else {
                    snprintf(status, sizeof(status), "  \xe2\x86\x92 %s", name);
                }
                fprintf(stderr, "\033[90m%s\033[0m\n", status);
                fflush(stderr);
            }

            char *result = llm_router_dispatch(&resp->tool_calls[i]);
            if (result) {
                stream_tool_output(result);
                if (result[0] && result[strlen(result)-1] != '\n')
                    stream_tool_output("\n");
                llm_history_add_tool_result(resp->tool_calls[i].id,
                                             resp->tool_calls[i].name,
                                             result);
                set_last_output(ctx, result);
            }
        }

        llm_response_free(resp);
        resp = NULL;

        if (interrupt_state) break;

        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
        resp = llm_chat_stream(NULL, cwd, ctx->last_output, NULL, 0, cbs);
    }

    if (resp && resp->num_tool_calls == 0 && resp->text && resp->text[0]) {
        trim_unsolicited_trailer(resp->text);
        stream_chat_output(resp->text);
        if (resp->text[strlen(resp->text) - 1] != '\n')
            stream_chat_output("\n");
        llm_history_add_assistant(resp->text);
    }

    if (iterations >= ctx->max_iterations)
        fprintf(stderr, "bash-llm: max iterations reached (%d)\n", ctx->max_iterations);

    llm_response_free(resp);
}

/* ---- Query ---- */

static void defer_chat_token(const char *token, void *userdata)
{
    (void)token;
    (void)userdata;
}

int llm_shell_query(llm_shell_ctx_t *ctx, const char *query, const char *context)
{
    /* A standalone greeting does not need context or a model request. */
    if (!context && (strcmp(query, "你好") == 0 ||
                     strcmp(query, "你好！") == 0 ||
                     strcmp(query, "你好!") == 0)) {
        llm_history_add_user(query);
        llm_history_add_assistant("你好");
        stream_chat_output("你好\n");
        return 0;
    }

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");

    char *full_query;
    if (context) {
        size_t len = strlen(query) + strlen(context) + 64;
        full_query = malloc(len);
        snprintf(full_query, len,
                 "[context]:\n%s\n\n[user request]: %s",
                 context, query);
    } else {
        full_query = strdup(query);
    }

    llm_history_add_user(full_query);
    streams_llm_active = 1;
    /* Wait for tool calls before deciding whether to print response text. */
    llm_stream_cbs cbs = ctx->cbs ? *ctx->cbs : (llm_stream_cbs){0};
    cbs.on_token = defer_chat_token;
    llm_response_t *resp = llm_chat_stream(full_query, cwd, ctx->last_output,
                                            NULL, 0, &cbs);
    free(full_query);

    if (!resp) {
        streams_llm_active = 0;
        fprintf(stderr, "bash-llm: LLM request failed\n");
        return 1;
    }

    int status = 0;
    if (!interrupt_state && ctx->max_iterations > 0 && resp->num_tool_calls == 1
        && llm_router_dispatch_direct(&resp->tool_calls[0], &status)) {
        const tool_call_t *call = &resp->tool_calls[0];
        char outcome[128];
        snprintf(outcome, sizeof(outcome), "Command output went directly to the shell. Exit status: %d.",
                 status);
        llm_history_add_assistant_tool_calls(NULL, call, 1);
        llm_history_add_tool_result(call->id, call->name, outcome);
        set_last_output(ctx, NULL);
        llm_response_free(resp);
    } else {
        llm_shell_agentic_loop(ctx, resp, &cbs);
    }
    streams_llm_active = 0;
    return status;
}
