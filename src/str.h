#ifndef STR_H_
#define STR_H_

int str_readline(FILE *, char *, int);

int str_split(char *, char, char **splits, int);
char *str_strip(char *, char);


#endif /* STR_H_ */
