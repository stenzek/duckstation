/*
 * Copyright (c) 2004, 2005, 2006, 2007, Svend Sorensen
 * Copyright (c) 2009, 2010 Jochen Keil
 * For license terms, see the file COPYING in this distribution.
 */

/* references: MMC-3 draft revsion - 10g */

#ifndef CDTEXT_H
#define CDTEXT_H

#include "libcue.h"

enum PtiFormat {
	FORMAT_CHAR,		/* single or double byte character string */
	FORMAT_BINARY		/* binary data */
};

/* return a pointer to a new Cdtext */
Cdtext *cdtext_init(void);

/* release a Cdtext */
void cdtext_delete(Cdtext *cdtext);

/* returns non-zero if there are no CD-TEXT fields set, zero otherwise */
int cdtext_is_empty(Cdtext *cdtext);

/* set CD-TEXT field to value for PTI pti */
void cdtext_set(int pti, char *value, Cdtext *cdtext);

/*
 * returns appropriate string for PTI pti
 * if istrack is zero, UPC/EAN string will be returned for PTI_UPC_ISRC
 * othwise ISRC string will be returned
 */
const char *cdtext_get_key(int pti, int istrack);

/*
 * dump all cdtext info
 * in human readable format (for debugging)
 */
void cdtext_dump(Cdtext *cdtext, int istrack);

#endif
