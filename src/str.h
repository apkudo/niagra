#ifndef STR_H_
#define STR_H_

int str_readline(FILE *, char *, int);

int str_split(char *, char, char **splits, int);
char *str_strip(char *, char);

int str_copy(char *, const char *, int);
int str_concat(char *, const char *, int);

int str_int(const char *, int *);
int str_uint16(const char *, uint16_t *);

/**
 * Return true if the string 'c' is zero-length. false
 * otherwise.
 */
static inline bool str_isempty(const char *c) {
    return c[0] =='\0';
}

#endif /* STR_H_ */
