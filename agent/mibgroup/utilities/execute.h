#ifndef _MIBGROUP_EXECUTE_H
#define _MIBGROUP_EXECUTE_H

config_belongs_in(agent_module);

int run_shell_command(const char *command, const char *input,
                      char *output, int *out_len);
int run_exec_command(const char *command, const char *input,
                     char *output, int *out_len);

#endif /* _MIBGROUP_EXECUTE_H */
