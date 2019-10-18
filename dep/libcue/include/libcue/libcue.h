/*
 * Copyright (c) 2009, 2010 Jochen Keil
 * For license terms, see the file COPYING in this distribution.
 */

#ifndef LIBCUE_H
#define LIBCUE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// #define CUE_EXPORT __attribute__((visibility("default")))
#define CUE_EXPORT

/*
 * disc modes
 * DATA FORM OF MAIN DATA (5.29.2.8)
 */
enum DiscMode {
	MODE_CD_DA,		/* CD-DA */
	MODE_CD_ROM,		/* CD-ROM mode 1 */
	MODE_CD_ROM_XA		/* CD-ROM XA and CD-I */
};

/*
 * track modes
 * 5.29.2.8 DATA FORM OF MAIN DATA
 * Table 350 - Data Block Type Codes
 */
enum TrackMode {
	MODE_AUDIO,		/* 2352 byte block length */
	MODE_MODE1,		/* 2048 byte block length */
	MODE_MODE1_RAW,		/* 2352 byte block length */
	MODE_MODE2,		/* 2336 byte block length */
	MODE_MODE2_FORM1,	/* 2048 byte block length */
	MODE_MODE2_FORM2,	/* 2324 byte block length */
	MODE_MODE2_FORM_MIX,	/* 2332 byte block length */
	MODE_MODE2_RAW		/* 2352 byte block length */
};

/*
 * sub-channel mode
 * 5.29.2.13 Data Form of Sub-channel
 * NOTE: not sure if this applies to cue files
 */
enum TrackSubMode {
	SUB_MODE_RW,		/* RAW Data */
	SUB_MODE_RW_RAW		/* PACK DATA (written R-W */
};

/*
 * track flags
 * Q Sub-channel Control Field (4.2.3.3, 5.29.2.2)
 */
enum TrackFlag {
	FLAG_NONE		= 0x00,	/* no flags set */
	FLAG_PRE_EMPHASIS	= 0x01,	/* audio recorded with pre-emphasis */
	FLAG_COPY_PERMITTED	= 0x02,	/* digital copy permitted */
	FLAG_DATA		= 0x04,	/* data track */
	FLAG_FOUR_CHANNEL	= 0x08,	/* 4 audio channels */
	FLAG_SCMS		= 0x10,	/* SCMS (not Q Sub-ch.) (5.29.2.7) */
	FLAG_ANY		= 0xff	/* any flags set */
};

enum DataType {
	DATA_AUDIO,
	DATA_DATA,
	DATA_FIFO,
	DATA_ZERO
};

/* cdtext pack type indicators */
enum Pti {
	PTI_TITLE,	/* title of album or track titles */
	PTI_PERFORMER,	/* name(s) of the performer(s) */
	PTI_SONGWRITER,	/* name(s) of the songwriter(s) */
	PTI_COMPOSER,	/* name(s) of the composer(s) */
	PTI_ARRANGER,	/* name(s) of the arranger(s) */
	PTI_MESSAGE,	/* message(s) from the content provider and/or artist */
	PTI_DISC_ID,	/* (binary) disc identification information */
	PTI_GENRE,	/* (binary) genre identification and genre information */
	PTI_TOC_INFO1,	/* (binary) table of contents information */
	PTI_TOC_INFO2,	/* (binary) second table of contents information */
	PTI_RESERVED1,	/* reserved */
	PTI_RESERVED2,	/* reserved */
	PTI_RESERVED3,	/* reserved */
	PTI_RESERVED4,	/* reserved for content provider only */
	PTI_UPC_ISRC,	/* UPC/EAN code of the album and ISRC code of each track */
	PTI_SIZE_INFO,	/* (binary) size information of the block */
	PTI_END		/* terminating PTI (for stepping through PTIs) */
};

enum RemType {
	REM_DATE,	/* date of cd/track */
	REM_REPLAYGAIN_ALBUM_GAIN,
	REM_REPLAYGAIN_ALBUM_PEAK,
	REM_REPLAYGAIN_TRACK_GAIN,
	REM_REPLAYGAIN_TRACK_PEAK,
	REM_END		/* terminating REM (for stepping through REMs) */
};

/* ADTs */
typedef struct Cd Cd;
typedef struct Track Track;
typedef struct Cdtext Cdtext;
typedef struct Rem Rem;
typedef enum Pti Pti;
typedef enum TrackFlag TrackFlag;
typedef enum RemType RemType;

CUE_EXPORT Cd* cue_parse_file(FILE*);
CUE_EXPORT Cd* cue_parse_string(const char*);
CUE_EXPORT void cd_delete(Cd* cd);

/* CD functions */
CUE_EXPORT enum DiscMode cd_get_mode(const Cd *cd);
CUE_EXPORT const char *cd_get_cdtextfile(const Cd *cd);
/*
 * return number of tracks in cd
 */
CUE_EXPORT int cd_get_ntrack(const Cd *cd);

/* CDTEXT functions */
CUE_EXPORT Cdtext *cd_get_cdtext(const Cd *cd);
CUE_EXPORT Cdtext *track_get_cdtext(const Track *track);
CUE_EXPORT const char *cdtext_get(enum Pti pti, const Cdtext *cdtext);

CUE_EXPORT Rem* cd_get_rem(const Cd* cd);
CUE_EXPORT Rem* track_get_rem(const Track* track);
/**
 * return pointer to value for rem comment
 * @param unsigned int: enum of rem comment
 */
CUE_EXPORT const char* rem_get(unsigned int, Rem*);

/* Track functions */
CUE_EXPORT Track *cd_get_track(const Cd *cd, int i);
CUE_EXPORT const char *track_get_filename(const Track *track);
CUE_EXPORT long track_get_start(const Track *track);
CUE_EXPORT long track_get_length(const Track *track);
CUE_EXPORT enum TrackMode track_get_mode(const Track *track);
CUE_EXPORT enum TrackSubMode track_get_sub_mode(const Track *track);
CUE_EXPORT int track_is_set_flag(const Track *track, enum TrackFlag flag);
CUE_EXPORT long track_get_zero_pre(const Track *track);
CUE_EXPORT long track_get_zero_post(const Track *track);
CUE_EXPORT const char *track_get_isrc(const Track *track);
CUE_EXPORT long track_get_index(const Track *track, int i);

#ifdef __cplusplus
}
#endif

#endif
