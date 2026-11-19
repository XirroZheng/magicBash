#ifndef BASH_LLM_SANDBOX_H
#define BASH_LLM_SANDBOX_H

/* A configured root enables sandboxed tool execution for this query. */
int llm_sandbox_begin(const char *root, const char *cwd);
int llm_sandbox_enabled(void);
const char *llm_sandbox_cwd(void);
char *llm_sandbox_execute(const char *name, const char *args, int *exit_status);
int llm_sandbox_finish(int allow_apply);

#endif
