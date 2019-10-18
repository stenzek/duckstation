%{
/*
 * Copyright (c) 2004, 2005, 2006, 2007, Svend Sorensen
 * Copyright (c) 2009, 2010 Jochen Keil
 * For license terms, see the file COPYING in this distribution.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cd.h"
#include "time.h"

#ifdef YY_BUF_SIZE
#undef YY_BUF_SIZE
#endif
#define YY_BUF_SIZE 16384

#define YYDEBUG 1

char fnamebuf[PARSER_BUFFER];

/* debugging */
//int yydebug = 1;

extern int yylineno;
extern FILE* yyin;

static Cd *cd = NULL;
static Track *track = NULL;
static Track *prev_track = NULL;
static Cdtext *cdtext = NULL;
static Rem *rem = NULL;
static char *prev_filename = NULL;	/* last file in or before last track */
static char *cur_filename = NULL;	/* last file in the last track */
static char *new_filename = NULL;	/* last file in this track */

/* lexer interface */
typedef struct yy_buffer_state* YY_BUFFER_STATE;

int yylex(void);
void yyerror(const char*);
YY_BUFFER_STATE yy_scan_string(const char*);
YY_BUFFER_STATE yy_create_buffer(FILE*, int);
void yy_switch_to_buffer(YY_BUFFER_STATE);
void yy_delete_buffer(YY_BUFFER_STATE);

/* parser interface */
int yyparse(void);
Cd *cue_parse_file(FILE *fp);
Cd *cue_parse_string(const char*);
%}

%start cuefile

%union {
	long ival;
	char *sval;
}

%token <ival> NUMBER
%token <sval> STRING

/* global (header) */
%token CATALOG
%token CDTEXTFILE

%token FFILE
%token BINARY
%token MOTOROLA
%token AIFF
%token WAVE
%token MP3
%token FLAC

/* track */
%token TRACK

%token <ival> AUDIO
%token <ival> MODE1_2048
%token <ival> MODE1_2352
%token <ival> MODE2_2336
%token <ival> MODE2_2048
%token <ival> MODE2_2342
%token <ival> MODE2_2332
%token <ival> MODE2_2352

/* ISRC is with CD_TEXT */
%token TRACK_ISRC

%token FLAGS
%token <ival> PRE
%token <ival> DCP
%token <ival> FOUR_CH
%token <ival> SCMS

%token PREGAP
%token INDEX
%token POSTGAP

/* CD-TEXT */
%token <ival> TITLE
%token <ival> PERFORMER
%token <ival> SONGWRITER
%token <ival> COMPOSER
%token <ival> ARRANGER
%token <ival> MESSAGE
%token <ival> DISC_ID
%token <ival> GENRE
%token <ival> TOC_INFO1
%token <ival> TOC_INFO2
%token <ival> UPC_EAN
%token <ival> ISRC
%token <ival> SIZE_INFO

%type <ival> track_mode
%type <ival> track_flag
%type <ival> time
%type <ival> cdtext_item

/* REM */
%type <ival> rem_item
%token <ival> DATE
%token <ival> XXX_GENRE /* parsed in REM but stored in CD-TEXT */
%token <ival> REPLAYGAIN_ALBUM_GAIN
%token <ival> REPLAYGAIN_ALBUM_PEAK
%token <ival> REPLAYGAIN_TRACK_GAIN
%token <ival> REPLAYGAIN_TRACK_PEAK
%%

cuefile
	: new_cd global_statements track_list
	;

new_cd
	: /* empty */ {
		cd = cd_init();
		cdtext = cd_get_cdtext(cd);
		rem = cd_get_rem(cd);
	}
	;

global_statements
	: /* empty */
	| global_statements global_statement
	;

global_statement
	: CATALOG STRING '\n' { cd_set_catalog(cd, $2); }
	| CDTEXTFILE STRING '\n' { cd_set_cdtextfile(cd, $2); }
	| cdtext
	| rem
	| track_data
	| error '\n'
	;

track_data
	: FFILE STRING file_format '\n' {
		if (NULL != new_filename) {
			yyerror("too many files specified\n");
		}
		if (track && track_get_index(track, 1) == -1) {
			track_set_filename (track, $2);
		} else {
			new_filename = strncpy(fnamebuf, $2, sizeof(fnamebuf));
			new_filename[sizeof(fnamebuf) - 1] = '\0';
		}
	}
	;

track_list
	: track
	| track_list track
	;

track
	: new_track track_def track_statements
	;

file_format
	: BINARY
	| MOTOROLA
	| AIFF
	| WAVE
	| MP3
	| FLAC
	;

new_track
	: /*empty */ {
		/* save previous track, to later set length */
		prev_track = track;

		track = cd_add_track(cd);
		cdtext = track_get_cdtext(track);
		rem = track_get_rem(track);

		cur_filename = new_filename;
		if (NULL != cur_filename)
			prev_filename = cur_filename;

		if (NULL == prev_filename)
			yyerror("no file specified for track");
		else
			track_set_filename(track, prev_filename);

		new_filename = NULL;
	}
	;

track_def
	: TRACK NUMBER track_mode '\n' {
		track_set_mode(track, $3);
	}
	;

track_mode
	: AUDIO
	| MODE1_2048
	| MODE1_2352
	| MODE2_2336
	| MODE2_2048
	| MODE2_2342
	| MODE2_2332
	| MODE2_2352
	;

track_statements
	: track_statement
	| track_statements track_statement
	;

track_statement
	: cdtext
	| rem
	| FLAGS track_flags '\n'
	| TRACK_ISRC STRING '\n' { track_set_isrc(track, $2); }
	| PREGAP time '\n' { track_set_zero_pre(track, $2); }
	| INDEX NUMBER time '\n' {
		long prev_length;

		/* Set previous track length if it has not been set */
		if (NULL != prev_track && NULL == cur_filename
		    && track_get_length (prev_track) == -1) {
			/* track shares file with previous track */
			prev_length = $3 - track_get_start(prev_track);
			track_set_length(prev_track, prev_length);
		}

		if (1 == $2) {
			/* INDEX 01 */
			track_set_start(track, $3);

			long idx00 = track_get_index (track, 0);

			if (idx00 != -1 && $3 != 0)
				track_set_zero_pre (track, $3 - idx00);
		}

		track_set_index (track, $2, $3);
	}
	| POSTGAP time '\n' { track_set_zero_post(track, $2); }
	| track_data
	| error '\n'
	;

track_flags
	: /* empty */
	| track_flags track_flag { track_set_flag(track, $2); }
	;

track_flag
	: PRE
	| DCP
	| FOUR_CH
	| SCMS
	;

cdtext
	: cdtext_item STRING '\n' { cdtext_set ($1, $2, cdtext); }
	;

cdtext_item
	: TITLE
	| PERFORMER
	| SONGWRITER
	| COMPOSER
	| ARRANGER
	| MESSAGE
	| DISC_ID
	| GENRE
	| TOC_INFO1
	| TOC_INFO2
	| UPC_EAN
	| ISRC
	| SIZE_INFO
	;

time
	: NUMBER
	| NUMBER ':' NUMBER ':' NUMBER { $$ = time_msf_to_frame($1, $3, $5); }
	;

rem
	: rem_item STRING '\n' { rem_set($1, $2, rem); }
	| XXX_GENRE STRING '\n' { cdtext_set($1, $2, cdtext); }
	;

rem_item
	: DATE
	| REPLAYGAIN_ALBUM_GAIN
	| REPLAYGAIN_ALBUM_PEAK
	| REPLAYGAIN_TRACK_GAIN
	| REPLAYGAIN_TRACK_PEAK
	;
%%

/* lexer interface */

void yyerror (const char *s)
{
	fprintf(stderr, "%d: %s\n", yylineno, s);
}

static void reset_static_vars()
{
	cd = NULL;
	track = NULL;
	prev_track = NULL;
	cdtext = NULL;
	rem = NULL;
	prev_filename = NULL;
	cur_filename = NULL;
	new_filename = NULL;
}

Cd *cue_parse_file(FILE *fp)
{
	YY_BUFFER_STATE buffer = NULL;

	yyin = fp;

	buffer = yy_create_buffer(yyin, YY_BUF_SIZE);

	yy_switch_to_buffer(buffer);

	Cd *ret_cd = NULL;

	if (0 == yyparse()) ret_cd = cd;
	else ret_cd = NULL;

	yy_delete_buffer(buffer);
	reset_static_vars();

	return ret_cd;
}

Cd *cue_parse_string(const char* string)
{
	YY_BUFFER_STATE buffer = NULL;

	buffer = yy_scan_string(string);

	Cd *ret_cd = NULL;

	if (0 == yyparse()) ret_cd = cd;
	else ret_cd = NULL;

	yy_delete_buffer(buffer);
	reset_static_vars();

	return ret_cd;
}
