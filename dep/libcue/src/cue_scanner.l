%{
/*
 * Copyright (c) 2004, 2005, 2006, 2007, Svend Sorensen
 * Copyright (c) 2009, 2010 Jochen Keil
 * For license terms, see the file COPYING in this distribution.
 */

#include <stdlib.h>
#include <string.h>

#include "cd.h"
#include "cue_parser.h"

char cueparser_buffer[PARSER_BUFFER];

int cueparserlex(void);
%}

ws		[ \t\r]
nonws		[^ \t\r\n]

%option yylineno
%option noyywrap
%option noinput
%option nounput
%option prefix="cueparser"

%s NAME
%x REM
%x RPG
%x SKIP

%%

\'([^\']|\\\')*\'	|
\"([^\"]|\\\")*\"	{
		cueparserlval.sval = strncpy(	cueparser_buffer,
					++yytext,
					(yyleng > sizeof(cueparser_buffer) ? sizeof(cueparser_buffer) : yyleng));
		cueparserlval.sval[(yyleng > sizeof(cueparser_buffer) ? sizeof(cueparser_buffer) : yyleng) - 2] = '\0';
		BEGIN(INITIAL);
		return STRING;
		}

<NAME>{nonws}+	{
		cueparserlval.sval = strncpy(	cueparser_buffer,
					yytext,
					(yyleng > sizeof(cueparser_buffer) ? sizeof(cueparser_buffer) : yyleng));
		cueparserlval.sval[(yyleng > sizeof(cueparser_buffer) ? sizeof(cueparser_buffer) : yyleng)] = '\0';
		BEGIN(INITIAL);
		return STRING;
		}

CATALOG		{ BEGIN(NAME); return CATALOG; }
CDTEXTFILE	{ BEGIN(NAME); return CDTEXTFILE; }

FILE		{ BEGIN(NAME); return FFILE; }
BINARY		{ return BINARY; }
MOTOROLA	{ return MOTOROLA; }
AIFF		{ return AIFF; }
WAVE		{ return WAVE; }
MP3		{ return MP3; }
FLAC		{ return FLAC; }

TRACK		{ return TRACK; }
AUDIO		{ cueparserlval.ival = MODE_AUDIO; return AUDIO; }
MODE1\/2048	{ cueparserlval.ival = MODE_MODE1; return MODE1_2048; }
MODE1\/2352	{ cueparserlval.ival = MODE_MODE1_RAW; return MODE1_2352; }
MODE2\/2336	{ cueparserlval.ival = MODE_MODE2; return MODE2_2336; }
MODE2\/2048	{ cueparserlval.ival = MODE_MODE2_FORM1; return MODE2_2048; }
MODE2\/2342	{ cueparserlval.ival = MODE_MODE2_FORM2; return MODE2_2342; }
MODE2\/2332	{ cueparserlval.ival = MODE_MODE2_FORM_MIX; return MODE2_2332; }
MODE2\/2352	{ cueparserlval.ival = MODE_MODE2_RAW; return MODE2_2352; }

FLAGS		{ return FLAGS; }
PRE		{ cueparserlval.ival = FLAG_PRE_EMPHASIS; return PRE; }
DCP		{ cueparserlval.ival = FLAG_COPY_PERMITTED; return DCP; }
4CH		{ cueparserlval.ival = FLAG_FOUR_CHANNEL; return FOUR_CH; }
SCMS		{ cueparserlval.ival = FLAG_SCMS; return SCMS; }

PREGAP		{ return PREGAP; }
INDEX		{ return INDEX; }
POSTGAP		{ return POSTGAP; }

TITLE		{ BEGIN(NAME); cueparserlval.ival = PTI_TITLE;  return TITLE; }
PERFORMER	{ BEGIN(NAME); cueparserlval.ival = PTI_PERFORMER;  return PERFORMER; }
SONGWRITER	{ BEGIN(NAME); cueparserlval.ival = PTI_SONGWRITER;  return SONGWRITER; }
COMPOSER	{ BEGIN(NAME); cueparserlval.ival = PTI_COMPOSER;  return COMPOSER; }
ARRANGER	{ BEGIN(NAME); cueparserlval.ival = PTI_ARRANGER;  return ARRANGER; }
MESSAGE		{ BEGIN(NAME); cueparserlval.ival = PTI_MESSAGE;  return MESSAGE; }
DISC_ID		{ BEGIN(NAME); cueparserlval.ival = PTI_DISC_ID;  return DISC_ID; }
GENRE		{ BEGIN(NAME); cueparserlval.ival = PTI_GENRE;  return GENRE; }
TOC_INFO1	{ BEGIN(NAME); cueparserlval.ival = PTI_TOC_INFO1;  return TOC_INFO1; }
TOC_INFO2	{ BEGIN(NAME); cueparserlval.ival = PTI_TOC_INFO2;  return TOC_INFO2; }
UPC_EAN		{ BEGIN(NAME); cueparserlval.ival = PTI_UPC_ISRC;  return UPC_EAN; }
ISRC/{ws}+\"	{ BEGIN(NAME); cueparserlval.ival = PTI_UPC_ISRC;  return ISRC; }
SIZE_INFO	{ BEGIN(NAME); cueparserlval.ival = PTI_SIZE_INFO;  return SIZE_INFO; }

ISRC		{ BEGIN(NAME); return TRACK_ISRC; }

REM		{ BEGIN(REM); /* exclusive rules for special exceptions */ }

<REM>DATE			{ BEGIN(NAME); cueparserlval.ival = REM_DATE; return DATE; }
<REM>GENRE			{ BEGIN(NAME); cueparserlval.ival = PTI_GENRE; return XXX_GENRE; }
<REM>REPLAYGAIN_ALBUM_GAIN 	{ BEGIN(RPG); cueparserlval.ival = REM_REPLAYGAIN_ALBUM_GAIN;
							return REPLAYGAIN_ALBUM_GAIN; }
<REM>REPLAYGAIN_ALBUM_PEAK	{ BEGIN(RPG); cueparserlval.ival = REM_REPLAYGAIN_ALBUM_PEAK;
							return REPLAYGAIN_ALBUM_PEAK; }
<REM>REPLAYGAIN_TRACK_GAIN	{ BEGIN(RPG); cueparserlval.ival = REM_REPLAYGAIN_TRACK_GAIN;
							return REPLAYGAIN_TRACK_GAIN; }
<REM>REPLAYGAIN_TRACK_PEAK	{ BEGIN(RPG); cueparserlval.ival = REM_REPLAYGAIN_TRACK_PEAK;
							return REPLAYGAIN_TRACK_PEAK; }

<REM>{ws}+	{ BEGIN(REM); }
<REM>.		{ BEGIN(REM); }
<REM>\n		{ BEGIN(INITIAL); }

<RPG>{nonws}+	{
		cueparserlval.sval = strncpy(	cueparser_buffer,
					yytext,
					(yyleng > sizeof(cueparser_buffer) ? sizeof(cueparser_buffer) : yyleng));
		cueparserlval.sval[(yyleng > sizeof(cueparser_buffer) ? sizeof(cueparser_buffer) : yyleng)] = '\0';
		BEGIN(SKIP);
		return STRING;
		}

<RPG>{ws}+	{ BEGIN(RPG); }

<SKIP>.*\n	{ BEGIN(INITIAL); yylineno++; return '\n'; }

{ws}+		{ /* ignore whitespace */ }

[[:digit:]]+	{ cueparserlval.ival = atoi(yytext); return NUMBER; }
:		{ return yytext[0]; }

^{ws}*\n	{ yylineno++; /* blank line */ }
\n		{ yylineno++; return '\n'; }
.		{ fprintf(stderr, "bad character '%c'\n", yytext[0]); }

%%
