#!/bin/sh
# Runs a command in the Switch build container (port/switch/docker), with
# the repository at /src. Without a command it configures and builds the
# game: tools/switch_docker.sh [--release]
#   tools/switch_docker.sh sh -c 'python3 configure.py && ninja switch'

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
image=halo-switch-build

docker build -q -t "$image" "$root/port/switch/docker" >/dev/null

if [ $# -eq 0 ] || [ "${1#-}" != "$1" ]; then
	set -- sh -c "python3 configure.py $* && ninja switch"
fi

tty=
[ -t 0 ] && tty=-it
exec docker run --rm $tty -v "$root:/src" -w /src "$image" "$@"
