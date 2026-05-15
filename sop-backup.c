#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINE_LEN 4096
#define MAX_TOKENS 64
#define DIR_MODE 0755

#include "copy.h"
#include "parser.h"
#include "restore.h"
#include "state.h"
#include "watch.h"
static volatile sig_atomic_t g_should_exit = 0;

static void parent_term_handler(int sig) {
  (void)sig;
  g_should_exit = 1;
}

static int dir_is_empty(const char* path) {
  DIR* d = opendir(path);
  if (!d) return -1;

  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    closedir(d);
    return 0;
  }
  closedir(d);
  return 1;
}

static int mkdir_p(const char* path) {
  char tmp[PATH_MAX];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(tmp)) return -1;

  strcpy(tmp, path);
  if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
  return 0;
}

static int is_target_inside_source(const char* source, const char* target) {
  char rs[PATH_MAX], rt[PATH_MAX];
  if (!realpath(source, rs)) return -1;
  if (!realpath(target, rt)) return -1;

  size_t ls = strlen(rs);
  if (strncmp(rt, rs, ls) != 0) return 0;
  if (rt[ls] == '\0' || rt[ls] == '/') return 1;
  return 0;
}

static void print_commands(void) {
  printf("Available commands:\n");
  printf("  add <source path> <target path> [<target path> ...]\n");
  printf("  end <source path> <target path> [<target path> ...]\n");
  printf("  restore <source path> <target path>\n");
  printf("  list\n");
  printf("  exit\n");
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = parent_term_handler;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  print_commands();

  while (1) {
    if (g_should_exit) {
      state_kill_all();
      while (waitpid(-1, NULL, 0) > 0) {
      }
      break;
    }
    state_cleanup_zombies();
    printf("> ");
    fflush(stdout);

    char line[MAX_LINE_LEN];
    if (!fgets(line, sizeof(line), stdin)) break;

    char* tokens[MAX_TOKENS];
    int ntokens = parse_command(line, tokens);
    if (ntokens == 0) {
      free_tokens(tokens, ntokens);
      continue;
    }

    if (strcmp(tokens[0], "exit") == 0) {
      free_tokens(tokens, ntokens);
      state_kill_all();
      while (waitpid(-1, NULL, 0) > 0) {
      }
      break;
    }

    else if (strcmp(tokens[0], "list") == 0) {
      state_list();
    }

    else if (strcmp(tokens[0], "add") == 0) {
      if (ntokens < 3) {
        printf("Error: add requires <source> <target> [...]\n");
        free_tokens(tokens, ntokens);
        continue;
      }

      const char* source = tokens[1];

      struct stat sst;
      if (lstat(source, &sst) < 0 || !S_ISDIR(sst.st_mode)) {
        printf("Error: source is not a directory\n");
        free_tokens(tokens, ntokens);
        continue;
      }

      for (int i = 2; i < ntokens; i++) {
        const char* target = tokens[i];

        struct stat tst;
        if (lstat(target, &tst) < 0) {
          if (mkdir_p(target) < 0) {
            perror("mkdir");
            continue;
          }
        } else {
          if (!S_ISDIR(tst.st_mode)) {
            printf("Error: target exists but is not a directory: \"%s\"\n",
                   target);
            continue;
          }
          int empty = dir_is_empty(target);
          if (empty <= 0) {
            printf("Error: target directory must be empty: \"%s\"\n", target);
            continue;
          }
        }

        int inside = is_target_inside_source(source, target);
        if (inside < 0) {
          perror("realpath");
          continue;
        }
        if (inside == 1) {
          printf("Error: target is inside source (not allowed)\n");
          continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
          perror("fork");
          continue;
        }

        if (pid == 0) {
          if (initial_copy(source, target) < 0) exit(1);
          watch_and_mirror(source, target);
          exit(0);
        }

        if (state_add(source, target, pid) == 0) {
          printf("Started backup: \"%s\" -> \"%s\" (pid %d)\n", source, target,
                 pid);
        } else {
          kill(pid, SIGTERM);
        }
      }
    }

    else if (strcmp(tokens[0], "end") == 0) {
      if (ntokens < 3) {
        printf("Error: end requires <source> <target> [...]\n");
        free_tokens(tokens, ntokens);
        continue;
      }

      for (int i = 2; i < ntokens; i++) {
        pid_t pid;
        if (state_remove_one(tokens[1], tokens[i], &pid) == 0) {
          kill(pid, SIGTERM);
          waitpid(pid, NULL, 0);
          printf("Stopped backup: \"%s\" -> \"%s\"\n", tokens[1], tokens[i]);
        } else {
          printf("Error: no such backup\n");
        }
      }
    }

    else if (strcmp(tokens[0], "restore") == 0) {
      if (ntokens != 3) {
        printf("Error: restore requires <source> <target>\n");
        free_tokens(tokens, ntokens);
        continue;
      } else {
        if (restore_backup(tokens[1], tokens[2]) == 0) {
          printf("Restore completed successfully\n");
        } else {
          perror("restore");
          printf("Error: restore failed\n");
        }
      }
    }

    else {
      printf("Error: unknown command\n");
    }
    free_tokens(tokens, ntokens);
  }
  return 0;
}
