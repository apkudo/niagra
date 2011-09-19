/* Copyright: Ben Leslie 2011: See LICENSE file. */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/**
 * Destructively split a string ('c') into multiple strings.
 *
 * A pointer to each new string is stored in the 'new_strings'
 * array.
 *
 * A maximum of 'max_new_strings' will be created. 'max_new_strings'
 * must be at least one. The array 'new_strings' must have at least
 * 'max_new_strings' elements.
 *
 * The string is split on the character 'sep'.
 *
 * The number of new strings that could be created is returned. This may
 * be more than 'max_new_strings'.
 *
 * If multiple adjacent 'sep' characters is encountered they are treated
 * as a single 'sep' character.
 *
 * This operation is destructive; the original string will no longer
 * be valid.
 */
int
str_split(char *c, char sep, char **new_strings, int max_new_strings)
{
    int num_new_strings = 1;

    new_strings[0] = c;

    while (*c != '\0') {
	if (*c == sep) {
	    if (num_new_strings < max_new_strings) {
		*c = '\0';
	    }
	    c++;
	    while (*c == sep) {
		c++;
	    }

	    if (num_new_strings < max_new_strings) {
		new_strings[num_new_strings] = c;
	    }
	    num_new_strings++;
	}
	c++;
    }

    return num_new_strings;
}

/**
 * Destructively remove any of the 'strip' characters from the
 * start and end of the string 'c'.
 *
 * The newly stripped string is returned.
 *
 * This operation is destructive; the original string will no longer
 * be valid.
 */
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

/**
 * Copy the source string 'str' in to the destination buffer 'dst'.
 *
 * The destination buffer has a maximum 'size'.
 *
 * Returns '0' on success.
 *
 * On failure returns '-1'.
 *
 * Failure can occur if the source string is too large to fit in the
 * destination buffer. The state of the destination buffer is
 * undefined in this case
 *
 * This is logically equivalent to:
 *
 * if (strlen(src) + 1 <= size) {
 *    strcpy(dst, src);
 *    return 0;
 * } else {
 *    return -1;
 * }
 */
int str_copy(char *dst, const char *src, int size)
{
    (void) strncpy(dst, src, size);
    return dst[size - 1] == '\0' ? 0 : -1;
}

/**
 * Concatented the source string 'str' in to the end of the string in
 * the destination buffer 'dst'.
 *
 * The destination buffer has a maximum 'size'.
 *
 * Returns '0' on success.
 *
 * On failure returns '-1'.
 *
 * Failure can occur if the concatenated string is too large to fit in
 * the destination buffer. The state of the destination buffer is
 * undefined in this case
 *
 * This is logically equivalent to:
 *
 * if (strlen(src) + 1 <= size) {
 *    strcat(dst, src);
 *    return 0;
 * } else {
 *    return -1;
 * }
 */
int str_concat(char *dst, const char *src, int size)
{
    (void) strncat(dst, src, size);
    return dst[size - 1] == '\0' ? 0 : -1;
}

/**
 * Parse an unsigned 16 bit integer from a 'str' in to 'val'. Return 0
 * on success and -1 on error.
 */
int str_uint16(const char *str, uint16_t *val) {
    unsigned long tmp;
    errno = 0;

    tmp = strtoul(str, NULL, 0);

    if (tmp > UINT16_MAX || tmp < 0) {
	errno = ERANGE;
	return -1;
    }

    if (errno != 0) {
	return -1;
    }

    *val = (uint16_t) tmp;

    return 0;
}

/**
 * Parse an signed integer from a 'str' in to 'val'. Return 0
 * on success and -1 on error.
 */
int str_int(const char *str, int *val) {
    long tmp;
    errno = 0;

    tmp = strtol(str, NULL, 0);

    if (tmp > INT_MAX || tmp < INT_MIN) {
	errno = ERANGE;
	return -1;
    }

    if (errno != 0) {
	return -1;
    }

    *val = (int) tmp;

    return 0;
}
