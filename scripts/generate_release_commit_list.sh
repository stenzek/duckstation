#!/usr/bin/env bash

if [ "$#" -ne 1 ]; then
	echo "Syntax: $0 <commit range, start..end>"
	exit 1
fi

IFS="
"

printf "## Commits\n"
for i in $(git log --oneline --reverse "$1"); do
	printf -- "- %s\n" "$i"
done

