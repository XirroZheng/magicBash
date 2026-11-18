#ifndef BASH_LLM_ROUTER_H
#define BASH_LLM_ROUTER_H

#include "llm_api.h"
#include "llm_serverconf.h"

/* Initialize router with config (for man enrichment settings) */
void llm_router_init(const server_config_t *conf);

/* Execute a supported direct-output tool call. Returns 1 if handled. */
int llm_router_dispatch_direct(const tool_call_t *tc, int *exit_status);

/*
 * Route a tool call to the appropriate handler.
 * Checks safety tier and prompts for confirmation if needed.
 * Returns malloced output string, or NULL on error/denial.
 */
char *llm_router_dispatch(const tool_call_t *tc);

#endif /* BASH_LLM_ROUTER_H */
