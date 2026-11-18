#ifndef BASH_LLM_EXEC_H
#define BASH_LLM_EXEC_H

/*
 * Execute an external command pipeline.
 * pipeline: array of command strings, e.g. {"grep -r TODO src/", "wc -l"}
 * n_cmds:   number of commands in the pipeline
 * stdin_file:  redirect stdin from file (NULL for none)
 * stdout_file: redirect stdout to file (NULL for none)
 * append:      if true, append to stdout_file instead of truncate
 * exit_status: if non-NULL, inherit stdout and write the last command's status
 *
 * Returns a malloced output or error string. In direct mode, NULL means the
 * pipeline was run and the outcome is available in exit_status.
 */
char *llm_exec_pipeline(const char **pipeline, int n_cmds,
                         const char *stdin_file, const char *stdout_file,
                         int append, int *exit_status);

#endif /* BASH_LLM_EXEC_H */
