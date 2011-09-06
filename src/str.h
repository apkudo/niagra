#ifndef STR_H_
#define STR_H_

int str_readline(FILE *stream, char *line, int max_len);
int str_split(char *line, char sep, char **splits, int max_split);
char *str_strip(char *c, char strip);

#endif /* STR_H_ */
