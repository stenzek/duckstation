/*
 * Copyright (c) 2004, 2005, 2006, 2007, Svend Sorensen
 * Copyright (c) 2009, 2010 Jochen Keil
 * For license terms, see the file COPYING in this distribution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cd.h"

typedef struct Data Data;
struct Data {
	int type;			/* DataType */
	char *name;			/* data source name */
	long start;			/* start time for data */
	long length;			/* length of data */
};

struct Track {
	Data zero_pre;			/* pre-gap generated with zero data */
	Data file;			/* track data file */
	Data zero_post;			/* post-gap generated with zero data */
	int mode;			/* track mode */
	int sub_mode;			/* sub-channel mode */
	int flags;			/* flags */
	char *isrc;			/* IRSC Code (5.22.4) 12 bytes */
	Cdtext *cdtext;			/* CD-TEXT */
	Rem* rem;
	long index[MAXINDEX+1];		/* indexes (in frames) (5.29.2.5)
					 * relative to start of file */
};

struct Cd {
	int mode;			/* disc mode */
	char *catalog;			/* Media Catalog Number (5.22.3) */
	char *cdtextfile;		/* Filename of CDText File */
	Cdtext *cdtext;			/* CD-TEXT */
	Rem* rem;
	int ntrack;			/* number of tracks in album */
	Track *track[MAXTRACK];		/* array of tracks */
};

Cd *cd_init(void)
{
	Cd *cd = NULL;
	cd = malloc(sizeof(Cd));

	if(NULL == cd) {
		fprintf(stderr, "unable to create cd\n");
	} else {
		cd->mode = MODE_CD_DA;
		cd->catalog = NULL;
		cd->cdtextfile = NULL;
		cd->cdtext = cdtext_init();
		cd->rem = rem_new();
		cd->ntrack = 0;
	}

	return cd;
}

void track_delete(struct Track* track)
{
	if (track != NULL)
	{
		cdtext_delete(track_get_cdtext(track));

		rem_free(track_get_rem(track));

		free(track->isrc);

		free(track->zero_pre.name);

		free(track->zero_post.name);

		free(track->file.name);

		free(track);
	}
}

void cd_delete(struct Cd* cd)
{
	int i = 0;

	if (cd != NULL)
	{
		free(cd->catalog);

		free(cd->cdtextfile);

		for (i = 0; i < cd->ntrack; i++)
			track_delete(cd->track[i]);

		cdtext_delete(cd_get_cdtext(cd));

		rem_free(cd_get_rem(cd));

		free(cd);
	}
}

Track *track_init(void)
{
	Track *track = NULL;
	track = malloc(sizeof(Track));

	if (NULL == track) {
		fprintf(stderr, "unable to create track\n");
	} else {
		track->zero_pre.type = DATA_ZERO;
		track->zero_pre.name = NULL;
		track->zero_pre.start = -1;
		track->zero_pre.length = -1;

		track->file.type = DATA_AUDIO;
		track->file.name = NULL;
		track->file.start = -1;
		track->file.length = -1;

		track->zero_post.type = DATA_ZERO;
		track->zero_post.name = NULL;
		track->zero_post.start = -1;
		track->zero_post.length = -1;

		track->mode = MODE_AUDIO;
		track->sub_mode = SUB_MODE_RW;
		track->flags = FLAG_NONE;
		track->isrc = NULL;
		track->cdtext = cdtext_init();
		track->rem = rem_new();

                int i;
                for (i=0; i<=MAXINDEX; i++)
                   track->index[i] = -1;
	}

	return track;
}

/*
 * cd structure functions
 */
void cd_set_mode(Cd *cd, int mode)
{
	cd->mode = mode;
}

enum DiscMode cd_get_mode(const Cd *cd)
{
	return cd->mode;
}

void cd_set_catalog(Cd *cd, char *catalog)
{
	if (cd->catalog)
		free(cd->catalog);

	cd->catalog = strdup(catalog);
}

void cd_set_cdtextfile(Cd *cd, char *cdtextfile)
{
	if (cd->cdtextfile)
		free(cd->cdtextfile);

	cd->cdtextfile = strdup(cdtextfile);
}

const char *cd_get_cdtextfile(const Cd *cd)
{
	return cd->cdtextfile;
}

Cdtext *cd_get_cdtext(const Cd *cd)
{
	if (cd != NULL)
		return cd->cdtext;
	else
		return NULL;
}

Rem*
cd_get_rem(const Cd* cd)
{
	if (cd != NULL)
		return cd->rem;
	else
		return NULL;
}

Track *cd_add_track(Cd *cd)
{
	if (MAXTRACK > cd->ntrack)
		cd->ntrack++;
	else
		fprintf(stderr, "too many tracks\n");

	/* this will reinit last track if there were too many */
	cd->track[cd->ntrack - 1] = track_init();

	return cd->track[cd->ntrack - 1];
}

int cd_get_ntrack(const Cd *cd)
{
	return cd->ntrack;
}

Track *cd_get_track(const Cd *cd, int i)
{
	if ((0 < i) && (i <= cd->ntrack) && (cd != NULL))
		return cd->track[i - 1];
	else
		return NULL;
}

/*
 * track structure functions
 */

void track_set_filename(Track *track, char *filename)
{
	if (track->file.name)
		free(track->file.name);

	track->file.name = strdup(filename);
}

const char *track_get_filename(const Track *track)
{
	return track->file.name;
}

void track_set_start(Track *track, long start)
{
	track->file.start = start;
}

long track_get_start(const Track *track)
{
	return track->file.start;
}

void track_set_length(Track *track, long length)
{
	track->file.length = length;
}

long track_get_length(const Track *track)
{
	return track->file.length;
}

void track_set_mode(Track *track, int mode)
{
	track->mode = mode;
}

enum TrackMode track_get_mode(const Track *track)
{
	return track->mode;
}

void track_set_sub_mode(Track *track, int sub_mode)
{
	track->sub_mode = sub_mode;
}

enum TrackSubMode track_get_sub_mode(const Track *track)
{
	return track->sub_mode;
}

void track_set_flag(Track *track, int flag)
{
	track->flags |= flag;
}

void track_clear_flag(Track *track, int flag)
{
	track->flags &= ~flag;
}

int track_is_set_flag(const Track *track, enum TrackFlag flag)
{
	return track->flags & flag;
}

void track_set_zero_pre(Track *track, long length)
{
	track->zero_pre.length = length;
}

long track_get_zero_pre(const Track *track)
{
	return track->zero_pre.length;
}

void track_set_zero_post(Track *track, long length)
{
	track->zero_post.length = length;
}

long track_get_zero_post(const Track *track)
{
	return track->zero_post.length;
}
void track_set_isrc(Track *track, char *isrc)
{
	if (track->isrc)
		free(track->isrc);

	track->isrc = strdup(isrc);
}

const char *track_get_isrc(const Track *track)
{
	return track->isrc;
}

Cdtext *track_get_cdtext(const Track *track)
{
	if (track != NULL)
		return track->cdtext;
	else
		return NULL;
}

Rem*
track_get_rem(const Track* track)
{
	if (track != NULL)
		return track->rem;
	else
		return NULL;
}

void track_set_index(Track *track, int i, long ind)
{
	if (i > MAXINDEX) {
		fprintf(stderr, "too many indexes\n");
                return;
        }

	track->index[i] = ind;
}

long track_get_index(const Track *track, int i)
{
	if ((0 <= i) && (i <= MAXINDEX))
		return track->index[i];

	return -1;
}

/*
 * dump cd information
 */
static void cd_track_dump(Track *track)
{
	int i;

	printf("zero_pre: %ld\n", track->zero_pre.length);
	printf("filename: %s\n", track->file.name);
	printf("start: %ld\n", track->file.start);
	printf("length: %ld\n", track->file.length);
	printf("zero_post: %ld\n", track->zero_post.length);
	printf("mode: %d\n", track->mode);
	printf("sub_mode: %d\n", track->sub_mode);
	printf("flags: 0x%x\n", track->flags);
	printf("isrc: %s\n", track->isrc);

	for (i = 0; i <= MAXINDEX; ++i)
                if (track->index[i] != -1)
                        printf("index %d: %ld\n", i, track->index[i]);

	if (NULL != track->cdtext) {
		printf("cdtext:\n");
		cdtext_dump(track->cdtext, 1);
	}

	if (track->rem != NULL)
	{
		printf("rem:\n");
		rem_dump(track->rem);
	}
}

void cd_dump(Cd *cd)
{
	int i;

	printf("Disc Info\n");
	printf("mode: %d\n", cd->mode);
	printf("catalog: %s\n", cd->catalog);
	printf("cdtextfile: %s\n", cd->cdtextfile);
	if (NULL != cd->cdtext) {
		printf("cdtext:\n");
		cdtext_dump(cd->cdtext, 0);
	}

	if (cd->rem != NULL)
	{
		printf("rem:\n");
		rem_dump(cd->rem);
	}

	for (i = 0; i < cd->ntrack; ++i) {
		printf("Track %d Info\n", i + 1);
		cd_track_dump(cd->track[i]);
	}
}
