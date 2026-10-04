#!/bin/sh
# Runs the Switch build in its container (port/switch/docker).
#
#   tools/switch_docker.sh [build] [configure.py options]
#       configures and builds the game: build/switch/halo.nro
#   tools/switch_docker.sh ninja <targets>
#       runs ninja in the build tree (after a build)
#   tools/switch_docker.sh nxlink <file.nro>
#       sends an .nro to the Homebrew Menu's netloader (press Y there) and
#       shows its output; the Switch connects back on port 28771. Set
#       SWITCH_IP to the Switch's address.
#   tools/switch_docker.sh <command>
#       runs a command with the repository at /src
#
# Builds run in a copy of the repository on a Docker volume: macOS file
# systems ignore case, and port/linux/include/StdDef.h would then stand in
# for <stddef.h>. The results are copied back to build/switch.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
image=halo-switch-build
volume=halo-switch-work

docker build -q -t "$image" "$root/port/switch/docker" >/dev/null

tty=
[ -t 0 ] && tty=-it

if [ $# -eq 0 ] || [ "${1#-}" != "$1" ] || [ "$1" = build ] || [ "$1" = ninja ]; then
	if [ "${1:-}" = build ]; then
		shift
	fi
	if [ "${1:-}" = ninja ]; then
		shift
		step="ninja $*"
	else
		step="python3 configure.py $* && ninja switch"
	fi
	exec docker run --rm $tty -v "$root:/src" -v "$volume:/work" -w /work "$image" sh -c "
		rsync -a --delete --exclude=/build --exclude=/build.ninja --exclude=/.ninja_* --exclude=/dist /src/ /work/ &&
		$step; status=\$?
		mkdir -p /src/build/switch &&
		find build/switch -maxdepth 1 -type f \\( -name '*.nro' -o -name '*.elf' -o -name '*.map' -o -name '*.nacp' \\) \
			-exec cp -p {} /src/build/switch/ \; 2>/dev/null
		exit \$status"
fi

ports=
if [ "$1" = nxlink ]; then
	shift
	ports="-p 28771:28771"
	set -- nxlink ${SWITCH_IP:+-a "$SWITCH_IP"} -s "$@"
fi

exec docker run --rm $tty $ports -v "$root:/src" -w /src "$image" "$@"
