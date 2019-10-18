/*
 * Copyright (c) 2004, 2005, 2006, 2007, Svend Sorensen
 * Copyright (c) 2009, 2010 Jochen Keil
 * For license terms, see the file COPYING in this distribution.
 */

/* references: MMC-3 draft revsion - 10g */

#ifndef CD_H
#define CD_H

#include "libcue.h"
#include "cdtext.h"
#include "rem.h"

#define MAXTRACK	99	/* Red Book track limit (from 01 to 99) */
#define MAXINDEX	99	/* Red Book index limit (from 00 to 99) */
#define PARSER_BUFFER   1024    /* Parser buffer size */


/* return pointer to CD structure */
Cd *cd_init(void);
Track *track_init(void);
void track_delete(struct Track* track);
void cd_dump(Cd *cd);

/*
 * Cd functions
 */

void cd_set_mode(Cd *cd, int mode);
void cd_set_catalog(Cd *cd, char *catalog);
void cd_set_cdtextfile(Cd *cd, char *cdtextfile);

/*
 * add a new track to cd, increment number of tracks
 * and return pointer to new track
 */
Track *cd_add_track(Cd *cd);

/*
 * Track functions
 */

/* filename of data file */
void track_set_filename(Track *track, char *filename);
/* track start is starting position in data file */
void track_set_start(Track *track, long start);
/* track length is length of data file to use */
void track_set_length(Track *track, long length);
/* see enum TrackMode */
void track_set_mode(Track *track, int mode);
/* see enum TrackSubMode */
void track_set_sub_mode(Track *track, int sub_mode);
/* see enum TrackFlag */
void track_set_flag(Track *track, int flag);
void track_clear_flag(Track *track, int flag);

void track_set_zero_pre(Track *track, long length);
void track_set_zero_post(Track *track, long length);
void track_set_isrc(Track *track, char *isrc);
void track_set_index(Track *track, int i, long index);

#endif
