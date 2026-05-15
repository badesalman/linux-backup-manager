#ifndef STATE_H
#define STATE_H

#include <sys/types.h>

int state_add(const char* source, const char* target, pid_t pid);
int state_remove_one(const char* source, const char* target, pid_t* out_pid);
void state_list(void);
void state_kill_all(void);
void state_cleanup_zombies(void);
#endif
