#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "extern.h"

char *
pgtypes_alloc(long size)
{
	char *new = (char *) calloc(1L, size);

	if (!new)
	{
		errno = ENOMEM;
		return NULL;
	}

	memset(new, '\0', size);
	return (new);
}

char *
pgtypes_strdup(char *str)
{
	char *new = (char *) strdup(str);

	if (!new)
		errno = ENOMEM;
	return (new);
}

int
pgtypes_fmt_replace(union un_fmt_replace replace_val, int replace_type, char** output, int *pstr_len) {
	/* general purpose variable, set to 0 in order to fix compiler
	 * warning */
	int i = 0;
	switch(replace_type) {
		case PGTYPES_REPLACE_NOTHING:
			break;
		case PGTYPES_REPLACE_STRING_CONSTANT:
		case PGTYPES_REPLACE_STRING_MALLOCED:
			i = strlen(replace_val.replace_str);
			if (i + 1 <= *pstr_len) {
				/* copy over i + 1 bytes, that includes the
				 * tailing terminator */
				strncpy(*output, replace_val.replace_str, i + 1);
				*pstr_len -= i;
				*output += i;
				if (replace_type == PGTYPES_REPLACE_STRING_MALLOCED) {
					free(replace_val.replace_str);
				}
				return 0;
			} else {
				return -1;
			}
			break;
		case PGTYPES_REPLACE_CHAR:
			if (*pstr_len >= 2) {
				(*output)[0] = replace_val.replace_char;
				(*output)[1] = '\0';
				(*pstr_len)--;
				(*output)++;
				return 0;
			} else {
				return -1;
			}
			break;
		case PGTYPES_REPLACE_DOUBLE_NF:
		case PGTYPES_REPLACE_INT64:
		case PGTYPES_REPLACE_UINT:
		case PGTYPES_REPLACE_UINT_2_LZ:
		case PGTYPES_REPLACE_UINT_2_LS:
		case PGTYPES_REPLACE_UINT_3_LZ:
		case PGTYPES_REPLACE_UINT_4_LZ:
			{
				char* t = pgtypes_alloc(PGTYPES_FMT_NUM_MAX_DIGITS);
				if (!t) {
					return ENOMEM;
				}
				switch (replace_type) {
					case PGTYPES_REPLACE_DOUBLE_NF:
						i = snprintf(t, PGTYPES_FMT_NUM_MAX_DIGITS,
								"%0.0g", replace_val.replace_double);
						break;
#ifdef HAVE_INT6
					case PGTYPES_REPLACE_INT64:
						i = snprintf(t, PGTYPES_FMT_NUM_MAX_DIGITS,
								INT64_FORMAT, replace_val.replace_int64);
						break;
#endif
					case PGTYPES_REPLACE_UINT:
							i = snprintf(t, PGTYPES_FMT_NUM_MAX_DIGITS,
									"%u", replace_val.replace_uint);
						break;
					case PGTYPES_REPLACE_UINT_2_LZ:
							i = snprintf(t, PGTYPES_FMT_NUM_MAX_DIGITS,
									"%02u", replace_val.replace_uint);
						break;
					case PGTYPES_REPLACE_UINT_2_LS:
							i = snprintf(t, PGTYPES_FMT_NUM_MAX_DIGITS,
									"%2u", replace_val.replace_uint);
						break;
					case PGTYPES_REPLACE_UINT_3_LZ:
							i = snprintf(t, PGTYPES_FMT_NUM_MAX_DIGITS,
									"%03u", replace_val.replace_uint);
						break;
					case PGTYPES_REPLACE_UINT_4_LZ:
							i = snprintf(t, PGTYPES_FMT_NUM_MAX_DIGITS,
									"%04u", replace_val.replace_uint);
						break;
				}

				if (i < 0) {
					free(t);
					return -1;
				}
				i = strlen(t);
				*pstr_len -= i;
				/* if *pstr_len == 0, we don't have enough
				 * space for the terminator and the
				 * conversion fails */
				if (*pstr_len <= 0) {
					free(t);
					return -1;
				}
				strcpy(*output, t);
				*output += i;
				free(t);
			}
			break;
		default:
			break;
	}
	return 0;
}


