/*-------------------------------------------------------------------------
 *
 * ts_public.h
 *	  Public interface to various tsearch modules, such as
 *	  parsers and dictionaries.
 *
 * Copyright (c) 1998-2007, PostgreSQL Global Development Group
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_TS_PUBLIC_H_
#define _PG_TS_PUBLIC_H_

#include "tsearch/ts_type.h"

/*
 * Parser's framework
 */

/*
 * returning type for prslextype method of parser
 */
typedef struct
{
	int			lexid;
	char	   *alias;
	char	   *descr;
} LexDescr;

/*
 * Interface to headline generator
 */
typedef struct
{
	uint32		selected:1,
				in:1,
				replace:1,
				repeated:1,
				unused:4,
				type:8,
				len:16;
	char	   *word;
	QueryItem  *item;
} HeadlineWordEntry;

typedef struct
{
	HeadlineWordEntry *words;
	int4		lenwords;
	int4		curwords;
	char	   *startsel;
	char	   *stopsel;
	int2		startsellen;
	int2		stopsellen;
} HeadlineParsedText;

/*
 * Common useful things for tsearch subsystem
 */

/* simple parser of cfg string looking like "key=val, key='val'" */
typedef struct
{
	char	   *key;
	char	   *value;
} Map;

extern void parse_keyvalpairs(text *in, Map ** m);

extern char *get_tsearch_config_filename(const char *basename,
										 const char *extension);

extern char *pnstrdup(const char *in, int len);

/*
 * Often useful stopword list management
 */
typedef struct
{
	int			len;
	char	  **stop;
	char	   *(*wordop) (char *);
} StopList;

extern void sortstoplist(StopList * s);
extern void readstoplist(char *in, StopList * s);
extern bool searchstoplist(StopList * s, char *key);

/*
 * Interface with dictionaries
 */

/* return struct for any lexize function */
typedef struct
{
	/*
	 * number of variant of split word , for example Word 'fotballklubber'
	 * (norwegian) has two varian to split: ( fotball, klubb ) and ( fot,
	 * ball, klubb ). So, dictionary should return: nvariant lexeme 1 fotball
	 * 1	  klubb 2	   fot 2	  ball 2	  klubb
	 */
	uint16		nvariant;

	uint16		flags;

	/* C-string */
	char	   *lexeme;
} TSLexeme;

#define TSL_ADDPOS		0x01

/*
 * Struct for supporting complex dictionaries like
 * thesaurus, pointer to is an 4-th argument for
 * dictlexize method
 */
typedef struct
{
	bool		isend;			/* in: marks for lexize_info about text end is
								 * reached */
	bool		getnext;		/* out: dict wants next lexeme */
	void	   *private;		/* internal dict state between calls with
								 * getnext == true */
} DictSubState;

#endif   /* _PG_TS_PUBLIC_H_ */