#ifndef PARSER_H
#define PARSER_H

#define MAX_TOKENS 64

int parse_command(const char *line, char *tokens[]);
void free_tokens(char *tokens[], int count);

#endif
