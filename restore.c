#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "restore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DIR_MODE 0755
#define FILE_MODE_MASK 0777
#define IO_BUF_SIZE 8192
#define NUL_TERM_SIZE 1

static int path_join(char* out, size_t out_sz, const char* a, const char* b) {
  if (!a || !b) return -1;
  size_t la = strlen(a), lb = strlen(b);
  if (la + 1 + lb + 1 > out_sz) return -1;

  memcpy(out, a, la);
  out[la] = '/';
  memcpy(out + la + 1, b, lb);
  out[la + 1 + lb] = '\0';
  return 0;
}

static int ensure_dir_exists(const char* path) {
  struct stat st;
  if (lstat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode)) return 0;
    if (unlink(path) < 0) return -1;
  }
  if (mkdir(path, DIR_MODE) < 0 && errno != EEXIST) return -1;
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

static int copy_file(const char* src, const char* dst, mode_t mode) {
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;

  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & FILE_MODE_MASK);
  if (out < 0) {
    close(in);
    return -1;
  }

  char buf[IO_BUF_SIZE];
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

static int starts_with(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int adjust_restore_symlink(char* out, size_t out_sz,
                                  const char* link_text,
                                  const char* source_root,
                                  const char* backup_root) {
  if (!link_text) return -1;  // eğer link_text null ise -1 döndür

  if (link_text[0] != '/') {  // eğer link_text mutlak bir yol değilse
    if (snprintf(out, out_sz, "%s", link_text) >= (int)out_sz)
      return -1;  // link_text'i out'a kopyala
    return 0;
  }

  char resolved[PATH_MAX];
  if (!realpath(link_text, resolved)) {
    if (snprintf(out, out_sz, "%s", link_text) >= (int)out_sz) return -1;
    return 0;
  }

  if (!starts_with(resolved, backup_root)) {
    if (snprintf(out, out_sz, "%s", link_text) >= (int)out_sz) return -1;
    return 0;
  }

  const char* rel = resolved + strlen(backup_root);
  if (*rel == '/') rel++;

  if (*rel == '\0') {
    if (snprintf(out, out_sz, "%s", source_root) >= (int)out_sz) return -1;
    return 0;
  }

  if (snprintf(out, out_sz, "%s/%s", source_root, rel) >= (int)out_sz)
    return -1;
  return 0;
}

static int read_symlink_text(const char* path, char* buf, size_t buf_sz) {
  ssize_t len = readlink(path, buf, buf_sz - NUL_TERM_SIZE);
  if (len < 0) return -1;
  buf[len] = '\0';
  return 0;
}

static int same_regular_file(const char* a, const char* b) {
  struct stat sa, sb;
  if (stat(a, &sa) < 0) return 0;
  if (stat(b, &sb) < 0) return 0;
  if (!S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode)) return 0;

  if (sa.st_size != sb.st_size) return 0;
  if (sa.st_mtime != sb.st_mtime) return 0;
  return 1;
}

static int same_symlink(const char* a, const char* b) {
  char ta[PATH_MAX], tb[PATH_MAX];
  if (read_symlink_text(a, ta, sizeof(ta)) < 0) return 0;
  if (read_symlink_text(b, tb, sizeof(tb)) < 0) return 0;
  return strcmp(ta, tb) == 0;
}

static int restore_from_backup(const char* source_root, const char* backup_root,
                               const char* backup_path,
                               const char* source_path) {
  struct stat st;
  if (lstat(backup_path, &st) < 0) return -1;

  if (S_ISDIR(st.st_mode)) {
    struct stat ss;
    if (lstat(source_path, &ss) == 0 && !S_ISDIR(ss.st_mode)) {
      remove_tree(source_path);
    }
    if (ensure_dir_exists(source_path) < 0) return -1;

    DIR* d = opendir(backup_path);
    if (!d) return -1;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;

      char bchild[PATH_MAX], schild[PATH_MAX];
      if (path_join(bchild, sizeof(bchild), backup_path, ent->d_name) < 0)
        continue;
      if (path_join(schild, sizeof(schild), source_path, ent->d_name) < 0)
        continue;

      if (restore_from_backup(source_root, backup_root, bchild, schild) < 0) {
        closedir(d);
        return -1;
      }
    }
    closedir(d);
    return 0;
  }

  if (S_ISREG(st.st_mode)) {
    struct stat ss;
    if (lstat(source_path, &ss) == 0) {
      if (S_ISREG(ss.st_mode) && same_regular_file(backup_path, source_path)) {
        return 0;
      }

      if (!S_ISREG(ss.st_mode)) {
        remove_tree(source_path);
      }
    }

    return copy_file(backup_path, source_path, st.st_mode);
  }

  if (S_ISLNK(st.st_mode)) {
    struct stat ss;
    if (lstat(source_path, &ss) == 0) {
      if (S_ISLNK(ss.st_mode) && same_symlink(backup_path, source_path)) {
        return 0;
      }
      remove_tree(source_path);
    }

    char link_text[PATH_MAX];
    if (read_symlink_text(backup_path, link_text, sizeof(link_text)) < 0)
      return -1;

    char adj[PATH_MAX];
    if (adjust_restore_symlink(adj, sizeof(adj), link_text, source_root,
                               backup_root) < 0)
      return -1;

    if (symlink(adj, source_path) < 0) return -1;
    return 0;
  }

  return 0;
}

static int delete_extras(const char* backup_root, const char* source_path,
                         const char* backup_path) {
  struct stat ss;
  if (lstat(source_path, &ss) < 0) return 0;

  struct stat bs;
  if (lstat(backup_path, &bs) < 0) {
    remove_tree(source_path);
    return 0;
  }

  if (S_ISDIR(ss.st_mode) && S_ISDIR(bs.st_mode)) {
    DIR* d = opendir(source_path);
    if (!d) return -1;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;

      char schild[PATH_MAX], bchild[PATH_MAX];
      if (path_join(schild, sizeof(schild), source_path, ent->d_name) < 0)
        continue;
      if (path_join(bchild, sizeof(bchild), backup_path, ent->d_name) < 0)
        continue;

      if (delete_extras(backup_root, schild, bchild) < 0) {
        closedir(d);
        return -1;
      }
    }
    closedir(d);
  }

  return 0;
}

int restore_backup(const char* source_root, const char* backup_root) {
  struct stat st;
  if (lstat(backup_root, &st) < 0 || !S_ISDIR(st.st_mode)) {
    errno = ENOENT;
    return -1;
  }

  if (ensure_dir_exists(source_root) < 0) return -1;

  if (restore_from_backup(source_root, backup_root, backup_root, source_root) <
      0)
    return -1;

  if (delete_extras(backup_root, source_root, backup_root) < 0) return -1;

  return 0;
}
