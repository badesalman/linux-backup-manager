#define _XOPEN_SOURCE 700

#include "copy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define COPY_BUF_SIZE 4096
#define DIR_MODE 0755
#define FILE_MODE 0644

static int copy_file(const char* src, const char* dst) {
  int in = open(src, O_RDONLY);  // open source file for reading
  if (in < 0) return -1;

  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC,
                 FILE_MODE);  // open/create dest file for writing
  if (out < 0) {
    close(in);
    return -1;
  }

  char buf[COPY_BUF_SIZE];  // buffer for copying data -bir  yerden başka yere
                            // aktarılırken hız farkı ya da zamanlama sorunu
                            // olmasın diye arada tutulan bellek alanı
  ssize_t r;
  while ((r = read(in, buf, sizeof(buf))) > 0) {  // read from source
    ssize_t w = write(out, buf, (size_t)r);
    if (w != r) {
      close(in);
      close(out);
      return -1;
    }
  }

  close(in);
  close(out);
  return 0;
}

static int is_link_to_self(
    const char* link_text,
    const char*
        source_path) {  // it checks whether a symlink points to its own file
  if (link_text[0] != '/') return 0;

  char resolved_link[PATH_MAX];    // linkin işaret ettiği gerçek yolu tutar
  char resolved_source[PATH_MAX];  // real path of symlink file

  if (realpath(link_text, resolved_link) == NULL) return 0;
  if (realpath(source_path, resolved_source) == NULL) return 0;
  // if they are same files, return 1 (true)
  return (strcmp(resolved_link, resolved_source) == 0);
}

int initial_copy(const char* source,
                 const char* target) {  // what we identify in header file
  struct stat st;
  if (lstat(source, &st) < 0) return -1;  // learns file type and other info

  if (S_ISDIR(st.st_mode)) {
    mkdir(target, 0755);
    // if its directory it opens and reads it
    DIR* dir = opendir(source);
    if (!dir) return -1;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
      if (strcmp(ent->d_name, ".") == 0 ||
          strcmp(ent->d_name, "..") ==
              0)  // skip . and .. mevcut klasör ve üst klasör
        continue;

      char src_path[PATH_MAX];
      char dst_path[PATH_MAX];

      snprintf(src_path, sizeof(src_path), "%s/%s", source, ent->d_name);
      snprintf(dst_path, sizeof(dst_path), "%s/%s", target, ent->d_name);

      initial_copy(src_path, dst_path);  // recursively calls to copy contents
    }
    closedir(dir);
  } else if (S_ISREG(st.st_mode)) {  // dosyayı hedefe kopyalar
    copy_file(source, target);
  } else if (S_ISLNK(st.st_mode)) {  // if its symlink
    char buf[PATH_MAX];
    ssize_t len =
        readlink(source, buf, sizeof(buf) - 1);  // read the symlink target
    if (len >= 0) {
      buf[len] = '\0';

      if (is_link_to_self(
              buf,
              source)) {  // if it points to itself, create a relative symlink
        char* filename = strrchr(source, '/');
        if (filename) {             // get the filename part
          char relative[PATH_MAX];  // create relative path
          snprintf(relative, sizeof(relative), ".%s", filename);
          if (symlink(relative, target) == 0) {  // create symlink

            return 0;
          }
        }
      }

      if (symlink(buf, target) < 0) {  // create symlink normally
        return -1;
      }
    }
  }

  return 0;
}