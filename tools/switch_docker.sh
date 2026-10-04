#!/bin/sh
# Runs a command in the Switch build container (port/switch/docker), with
# the repository at /src. Without a command it configures and builds the
# game: tools/switch_docker.sh [--release]
#   tools/switch_docker.sh sh -c 'python3 configure.py && ninja switch'
# nxlink sends an .nro to the Homebrew Menu's netloader (press Y there) and
# shows the program's output; the Switch connects back on port 28771:
#   SWITCH_IP=192.168.3.114 tools/switch_docker.sh nxlink dist/switch/probe.nro

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
image=halo-switch-build

docker build -q -t "$image" "$root/port/switch/docker" >/dev/null

if [ $# -eq 0 ] || [ "${1#-}" != "$1" ]; then
	set -- sh -c "python3 configure.py $* && ninja switch"
fi

ports=
if [ "$1" = nxlink ]; then
	shift
	ports="-p 28771:28771"
	set -- nxlink ${SWITCH_IP:+-a "$SWITCH_IP"} -s "$@"
fi

tty=
[ -t 0 ] && tty=-it
exec docker run --rm $tty $ports -v "$root:/src" -w /src "$image" "$@"
