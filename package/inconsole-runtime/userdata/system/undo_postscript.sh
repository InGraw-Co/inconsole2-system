#!/bin/sh
# Called during factory reset before profile/settings files are removed.
set -eu

echo "Removing postscript artifacts..."
# Add your cleanup commands below, for example:
# rm -rf /path/to/generated/cache
# userdel -r customuser || true

echo "Undo cleanup completed."
exit 0
