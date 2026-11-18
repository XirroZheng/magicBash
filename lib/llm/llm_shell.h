#ifndef BASH_LLM_SHELL_H
#define BASH_LLM_SHELL_H

#include "llm_api.h"
#include "llm_serverconf.h"

/*
 * Shell context -- holds all state for the LLM subsystem.
 */
typedef struct {
    server_config_t *servers;
    llm_stream_cbs *cbs;
    char *last_output;      /* last tool output for LLM context */
    int max_iterations;
} llm_shell_ctx_t;

/*
 * Send a query to the LLM with optional piped context and run the
 * agentic loop or a direct-output command. Returns a shell exit status.
 */
int llm_shell_query(llm_shell_ctx_t *ctx, const char *query, const char *context);

#endif /* BASH_LLM_SHELL_H */
