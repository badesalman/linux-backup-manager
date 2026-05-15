#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "watch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DIR_MODE 0755
#define FILE_MODE 0644

static int path_join(
    char* out, size_t out_sz, const char* a,
    const char* b) {  // bu fonksiyon, iki dosya yolunu birleştirir ve sonucu
                      // 'out' değişkenine yazar.
  if (!a || !b) return -1;

  size_t la = strlen(a);
  size_t lb = strlen(b);

  if (la + 1 + lb + 1 > out_sz) return -1;

  memcpy(out, a, la);
  out[la] = '/';
  memcpy(out + la + 1, b, lb);
  out[la + 1 + lb] = '\0';
  return 0;
}

static int make_target_path(char* out, size_t out_sz, const char* src_root,
                            const char* tgt_root, const char* src_path) {
  size_t n = strlen(src_root);
  if (strncmp(src_path, src_root, n) != 0) return -1;

  const char* rel = src_path + n;
  if (*rel == '/') rel++;
  if (*rel == '\0') {
    if (snprintf(out, out_sz, "%s", tgt_root) >= (int)out_sz) return -1;
    return 0;
  }
  if (snprintf(out, out_sz, "%s/%s", tgt_root, rel) >= (int)out_sz) return -1;
  return 0;
}

static void remove_tree(const char* path) {
  struct stat st;
  if (lstat(path, &st) < 0) return;

  if (S_ISDIR(st.st_mode)) {
    DIR* d = opendir(path);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;

      char child[PATH_MAX];
      if (path_join(child, sizeof(child), path, ent->d_name) == 0) {
        remove_tree(child);
      }
    }
    closedir(d);
    rmdir(path);
  } else {
    unlink(path);
  }
}

static int copy_file(const char* src, const char* dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;

  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, FILE_MODE);
  if (out < 0) {
    close(in);
    return -1;
  }

  char buf[BUFSIZ];
  ssize_t r;
  while ((r = read(in, buf, sizeof(buf))) > 0) {
    ssize_t w = write(out, buf, (size_t)r);
    if (w != r) {
      close(in);
      close(out);
      return -1;
    }
  }

  close(in);
  close(out);
  return (r < 0) ? -1 : 0;
}

static int mkdir_p_parent(const char* path) {
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;

  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      (void)mkdir(tmp, DIR_MODE);
      *p = '/';
    }
  }
  return 0;
}

static int starts_with(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int adjust_symlink(char* out, size_t out_sz, const char* link_text,
                          const char* src_path, const char* src_root,
                          const char* tgt_root) {
  if (!link_text) return -1;

  if (link_text[0] == '/') {
    char resolved_link[PATH_MAX];
    char resolved_source[PATH_MAX];

    if (realpath(link_text, resolved_link)) {
      if (realpath(src_path, resolved_source)) {
        if (strcmp(resolved_link, resolved_source) == 0) {
          const char* rel = src_path + strlen(src_root);
          if (*rel == '/') rel++;

          if (snprintf(out, out_sz, "%s/%s", tgt_root, rel) >= (int)out_sz)
            return -1;
          return 0;
        }
      }
    }
  }

  if (link_text[0] != '/') {
    if (snprintf(out, out_sz, "%s", link_text) >= (int)out_sz) return -1;
    return 0;
  }

  char resolved[PATH_MAX];
  if (!realpath(link_text, resolved)) {
    if (snprintf(out, out_sz, "%s", link_text) >= (int)out_sz) return -1;
    return 0;
  }

  if (!starts_with(resolved, src_root)) {
    if (snprintf(out, out_sz, "%s", link_text) >= (int)out_sz) return -1;
    return 0;
  }

  if (make_target_path(out, out_sz, src_root, tgt_root, resolved) < 0) {
    if (snprintf(out, out_sz, "%s", link_text) >= (int)out_sz) return -1;
  }
  return 0;
}

static int sync_path(const char* src_root, const char* tgt_root,
                     const char* src_path) {
  char tgt_path[PATH_MAX];
  if (make_target_path(tgt_path, sizeof(tgt_path), src_root, tgt_root,
                       src_path) < 0)
    return -1;

  struct stat st;
  if (lstat(src_path, &st) < 0) {
    remove_tree(tgt_path);
    return 0;
  }

  if (S_ISDIR(st.st_mode)) {
    (void)mkdir(tgt_path, DIR_MODE);  // ignore if exists
    return 0;
  }

  mkdir_p_parent(tgt_path);

  if (S_ISREG(st.st_mode)) {
    return copy_file(src_path, tgt_path);
  }

  if (S_ISLNK(st.st_mode)) {
    char buf[PATH_MAX];
    ssize_t len = readlink(src_path, buf, sizeof(buf) - 1);
    if (len < 0) return -1;
    buf[len] = '\0';

    remove_tree(tgt_path);

    char adj[PATH_MAX];
    if (adjust_symlink(adj, sizeof(adj), buf, src_path, src_root, tgt_root) < 0)
      return -1;
    if (symlink(adj, tgt_path) < 0) return -1;
    return 0;
  }

  return 0;
}

typedef struct {
  int wd;
  char path[PATH_MAX];
} WatchEnt;

static WatchEnt* W = NULL;
static size_t W_len = 0;

static const char* wd_to_path(int wd) {
  for (size_t i = 0; i < W_len; i++)
    if (W[i].wd == wd) return W[i].path;
  return NULL;
}

static void remove_wd(int wd) {
  for (size_t i = 0; i < W_len; i++) {
    if (W[i].wd == wd) {
      W[i] = W[W_len - 1];
      W_len--;
      return;
    }
  }
}

static uint32_t watch_mask(void) {
  return IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM |
         IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
}

static int add_watch_dir(int fd, const char* dirpath) {
  int wd = inotify_add_watch(fd, dirpath, watch_mask());
  if (wd < 0) return -1;

  WatchEnt* nw = (WatchEnt*)realloc(W, (W_len + 1) * sizeof(WatchEnt));
  if (!nw) return -1;
  W = nw;

  W[W_len].wd = wd;
  snprintf(W[W_len].path, sizeof(W[W_len].path), "%s", dirpath);
  W_len++;
  return 0;
}

static int add_watch_recursive(int fd, const char* root) {
  if (add_watch_dir(fd, root) < 0) return -1;

  DIR* d = opendir(root);
  if (!d) return -1;

  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;

    char child[PATH_MAX];
    if (path_join(child, sizeof(child), root, ent->d_name) < 0) continue;

    struct stat st;
    if (lstat(child, &st) < 0) continue;

    if (S_ISDIR(st.st_mode)) {
      (void)add_watch_recursive(fd, child);
    }
  }

  closedir(d);
  return 0;
}

static volatile sig_atomic_t g_run = 1;

static void term_handler(int sig) {
  (void)sig;
  g_run = 0;
}

int watch_and_mirror(const char* src_root, const char* tgt_root) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = term_handler;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  int fd = inotify_init1(0);
  if (fd < 0) return -1;

  if (add_watch_recursive(fd, src_root) < 0) {
    close(fd);
    return -1;
  }

  char buf[BUFSIZ];

  while (g_run) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    ssize_t i = 0;
    while (i < n) {
      struct inotify_event* ev = (struct inotify_event*)(buf + i);
      const char* base = wd_to_path(ev->wd);

      if (base) {
        char src_path[PATH_MAX];

        if (ev->len > 0 && ev->name[0] != '\0') {
          (void)path_join(src_path, sizeof(src_path), base, ev->name);
        } else {
          (void)snprintf(src_path, sizeof(src_path), "%s", base);
        }

        if (ev->mask & (IN_CREATE | IN_MODIFY | IN_ATTRIB | IN_MOVED_TO)) {
          (void)sync_path(src_root, tgt_root, src_path);

          if (ev->mask & IN_ISDIR) {
            (void)add_watch_recursive(fd, src_path);
          }
        }

        if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
          char tgt_path[PATH_MAX];
          if (make_target_path(tgt_path, sizeof(tgt_path), src_root, tgt_root,
                               src_path) == 0) {
            remove_tree(tgt_path);
          }
        }

        if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
          remove_wd(ev->wd);

          if (base && strcmp(base, src_root) == 0) {
            g_run = 0;
            break;
          }
        }
      }

      i += (ssize_t)sizeof(struct inotify_event) + ev->len;
    }
  }

  free(W);
  W = NULL;
  W_len = 0;

  close(fd);
  return 0;
}
