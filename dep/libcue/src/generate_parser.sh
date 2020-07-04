#!/bin/sh

flex --nounistd -o cue_scanner.c cue_scanner.l
bison --defines=cue_parser.h -o cue_parser.c cue_parser.y

