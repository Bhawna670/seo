#!/bin/sh
# One private history file per visitor session, so visitors don't see each other's searches.
H=$(mktemp /tmp/history.XXXXXX)
exec ./search_engine Data "$H"
