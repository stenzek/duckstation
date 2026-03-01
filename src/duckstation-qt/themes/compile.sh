#!/usr/bin/env bash

set -e

SCRIPTDIR=$(dirname "${BASH_SOURCE[0]}")
RESDIR=$(realpath "$SCRIPTDIR/../resources/themes")

if [ ! -d "$RESDIR" ]; then
	echo "Error: $RESDIR does not exist." >&2
	exit 1
fi

# Ensure we're using dart sass, not ruby sass.
sass --version | grep -q "dart" || {
	echo "Error: Dart Sass is required" >&2
	exit 1
}

IFS="
"

# Avoid different platforms generating different order.
export LC_ALL=C

for i in $(find . -name '*.scss' -not -name '_template.scss'); do
	name=$(basename "$i")
	outname=$(basename "$i" .scss).qss
	echo "Generating $outname from $name"
	sass --style=compressed --no-source-map "$i" "$RESDIR/$outname"
done
