#define _POSIX_C_SOURCE 200809L
#include "state.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define INITIAL_CAPACITY 8
#define GROWTH_FACTOR 2
#define NUL_TERM_SIZE 1

typedef struct {
  char* source;
  char* target;
  pid_t pid;
} BackupEntry;

typedef struct {
  BackupEntry* entries;
  size_t len;
  size_t cap;
} BackupState;

static BackupState g_state = {NULL, 0, 0};

static char* dupstr(const char* s) {
  size_t n = strlen(s);
  char* p = (char*)malloc(n + NUL_TERM_SIZE);
  if (!p) return NULL;
  memcpy(p, s, n + NUL_TERM_SIZE);
  return p;
}

static int ensure_cap(size_t need) {
  if (g_state.cap >= need) return 0;
  size_t new_cap =
      (g_state.cap == 0) ? INITIAL_CAPACITY : g_state.cap * GROWTH_FACTOR;
  while (new_cap < need) new_cap *= GROWTH_FACTOR;

  BackupEntry* ng =
      (BackupEntry*)realloc(g_state.entries, new_cap * sizeof(BackupEntry));
  if (!ng) return -1;

  g_state.entries = ng;
  g_state.cap = new_cap;
  return 0;
}

static int is_duplicate(const char* source, const char* target) {
  for (size_t i = 0; i < g_state.len; i++) {
    if (strcmp(g_state.entries[i].source, source) == 0 &&
        strcmp(g_state.entries[i].target, target) == 0)
      return 1;
  }
  return 0;
}

int state_add(const char* source, const char* target, pid_t pid) {
  if (is_duplicate(source, target)) return -2;

  if (ensure_cap(g_state.len + 1) < 0) return -1;

  g_state.entries[g_state.len].source = dupstr(source);
  g_state.entries[g_state.len].target = dupstr(target);
  if (!g_state.entries[g_state.len].source ||
      !g_state.entries[g_state.len].target) {
    free(g_state.entries[g_state.len].source);
    free(g_state.entries[g_state.len].target);
    return -1;
  }

  g_state.entries[g_state.len].pid = pid;
  g_state.len++;
  return 0;
}

int state_remove_one(const char* source, const char* target, pid_t* out_pid) {
  for (size_t i = 0; i < g_state.len; i++) {
    if (strcmp(g_state.entries[i].source, source) == 0 &&
        strcmp(g_state.entries[i].target, target) == 0) {
      if (out_pid) *out_pid = g_state.entries[i].pid;

      free(g_state.entries[i].source);
      free(g_state.entries[i].target);

      g_state.entries[i] = g_state.entries[g_state.len - 1];
      g_state.len--;
      return 0;
    }
  }
  return -1;
}

void state_list(void) {
  if (g_state.len == 0) {
    printf("Active backups: (none)\n");
    return;
  }

  printf("Active backups:\n");
  for (size_t i = 0; i < g_state.len; i++) {
    printf("  \"%s\" -> \"%s\" (pid %d)\n", g_state.entries[i].source,
           g_state.entries[i].target, (int)g_state.entries[i].pid);
  }
}

void state_kill_all(void) {
  for (size_t i = 0; i < g_state.len; i++) {
    kill(g_state.entries[i].pid, SIGTERM);
    free(g_state.entries[i].source);
    free(g_state.entries[i].target);
  }

  free(g_state.entries);
  g_state.entries = NULL;
  g_state.len = 0;
  g_state.cap = 0;
}
void state_cleanup_zombies(void) {
  pid_t pid;
  int status;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (size_t i = 0; i < g_state.len; i++) {
      if (g_state.entries[i].pid == pid) {
        printf("Cleaned up zombie process %d (backup: %s -> %s)\n", (int)pid,
               g_state.entries[i].source, g_state.entries[i].target);

        free(g_state.entries[i].source);
        free(g_state.entries[i].target);

        g_state.entries[i] = g_state.entries[g_state.len - 1];
        g_state.len--;
        break;
      }
    }
  }
}