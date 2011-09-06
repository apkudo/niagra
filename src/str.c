/* Copyright: Ben Leslie 2011: See LICENSE file. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "str.h"

/**
 * reads a line of input from the stream into the 'line' buffer.
 *
 * On error return -1.
 * On end of file return 0.
 * On success return number of characters in the line. Excluding terminating '\0'
 *
 * The line will never include the line-terminate ('\n')
 *
 * If the line does not fit in the buffer then errno will be
 * set to E2BIG. Additionally, any errors that fgets() may
 * set errno to.
 */
/* FIXME: It might be easier to make this function operate on a direct
 * fd, rather than a FILE* object! */
int
str_readline(FILE *stream, char *line, int max_len)
{
    char *r;
    int len;

    r = fgets(line, max_len, stream);

    if (r == NULL)  {
	/* EOF or Error */
	if (ferror(stream)) {
	    return -1;
	} else {
	    line[0] = '\0';
	    return 0;
	}
    }

    len = strlen(line); /* excludes terminating \0 */

    if (len + 1 == max_len && line[len - 1] != '\n' && !feof(stream)) {
	errno = E2BIG;
	return -1;
    }

    if (line[len - 1] == '\n') {
	line[len - 1] = '\0';
	len--;
    }

    return len;
}

int
str_split(char *c, char sep, char **splits, int max_split)
{
    int split = 1;

    splits[0] = c;

    while (*c != '\0') {
	if (*c == sep) {
	    if (split < max_split) {
		*c = '\0';
	    }
	    c++;
	    while (*c == sep) {
		c++;
	    }

	    if (split < max_split) {
		splits[split] = c;
	    }
	    split++;
	}
	c++;
    }

    return split;
}

char *
str_strip(char *c, char strip)
{
    char *start = c;

    /* Find the start of the string */
    while (*start == strip) {
	start++;
    }

    c = c + strlen(c) - 1;

    if (*c == strip) {
	while (*c == strip) {
	    c--;
	}
	c++;
	*c = '\0';
    }

    return start;
}
