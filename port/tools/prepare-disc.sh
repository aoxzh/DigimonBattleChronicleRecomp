#!/bin/sh
# Import an RVZ (or validate ISO/GCM) for the desktop runner.
exec python3 "$(dirname "$0")/prepare_disc.py" "$@"
