#include "parser.h"

#include <ctype.h>   // for isspace
#include <stdlib.h>  // for malloc
#include <string.h>  // for strncpy

int parse_command(const char* line, char* tokens[]) {
  int count = 0;
  const char* p = line;

  while (*p) {
    while (isspace(*p)) p++;  // skip whitespace
    if (*p == '\0' || *p == '\n') break;

    if (*p == '"') {  // quoted token
      p++;
      const char* start = p;
      while (*p && *p != '"') p++;
      int len = p - start;

      tokens[count] = malloc(len + 1);     // allocate memory for the token
      strncpy(tokens[count], start, len);  // copy the token
      tokens[count][len] = '\0';
      count++;

      if (*p == '"') p++;
    } else {  // unquoted token
      const char* start = p;
      while (*p && !isspace(*p)) p++;
      int len = p - start;

      tokens[count] = malloc(len + 1);
      strncpy(tokens[count], start, len);
      tokens[count][len] = '\0';
      count++;
    }

    if (count >= MAX_TOKENS) break;
  }

  tokens[count] = NULL;  // klasik argv gibi nullla bitiriyoruz
  return count;
}

void free_tokens(char* tokens[], int count) {
  for (int i = 0; i < count; i++) {
    free(tokens[i]);
  }
}
