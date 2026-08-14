#!/usr/bin/env bash
#
# Build wii64-ps3 inside the Docker toolchain image.
#
#   ./docker/build.sh            # build the .self
#   ./docker/build.sh pkg        # build an installable .pkg
#   ./docker/build.sh clean      # wipe the cached object directory
#
# Why the tar pipe instead of `docker run -v "$PWD:/src"`:
# Docker Desktop's VirtioFS mount on this setup cannot read pre-existing host
# files from inside the container -- every read fails with EDEADLK
# ("Resource deadlock avoided") while files created in the container read fine.
# Piping a tar of the tree in sidesteps the mount entirely. The source is only
# a few MB, so it costs about a second.
#
# Object files live in the named volume `wii64-ps3-build` mounted at build/, so
# rebuilds are incremental: tar preserves mtimes, so make only recompiles what
# actually changed.

set -euo pipefail

IMAGE="${IMAGE:-wii64-ps3-build:latest}"
VOLUME="${VOLUME:-wii64-ps3-build}"
TARGET="ps364_glN64"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS="${JOBS:-$(docker run --rm "$IMAGE" nproc 2>/dev/null || echo 2)}"

cd "$REPO_ROOT"

if [ "${1:-}" = "clean" ]; then
	docker volume rm -f "$VOLUME" >/dev/null 2>&1 || true
	rm -f "$TARGET".elf "$TARGET".self "$TARGET"*.pkg
	echo "cleaned object cache and output artifacts"
	exit 0
fi

GOAL="${1:-}"
case "$GOAL" in
	pkg) MAKE_GOAL="pkg" ;;
	"")  MAKE_GOAL="" ;;
	*)   MAKE_GOAL="$GOAL" ;;
esac

echo ">> building $TARGET ${MAKE_GOAL:+($MAKE_GOAL) }with -j$JOBS"

# Artifacts are tarred back to stdout on fd 3 so they stay separate from the
# build log on stdout/stderr.
COPYFILE_DISABLE=1 tar -cf - \
	--exclude=.git --exclude=build --exclude=docker --exclude='*.self' \
	--exclude='*.elf' --exclude='*.pkg' . \
| docker run --rm -i \
	-v "$VOLUME:/src/wii64/build" \
	"$IMAGE" \
	sh -c "tar -xf - 2>/dev/null \
	       && make -f Makefile.ps3 -j$JOBS $MAKE_GOAL >&2 \
	       && tar -cf - --ignore-failed-read \
	            $TARGET.elf $TARGET.self $TARGET*.pkg 2>/dev/null" \
> /tmp/wii64-artifacts.tar

if [ -s /tmp/wii64-artifacts.tar ]; then
	tar -xf /tmp/wii64-artifacts.tar -C "$REPO_ROOT"
	rm -f /tmp/wii64-artifacts.tar
	echo ">> artifacts:"
	ls -lh "$TARGET".elf "$TARGET".self "$TARGET"*.pkg 2>/dev/null | awk '{print "   ", $9, $5}'
else
	echo ">> build produced no artifacts" >&2
	exit 1
fi
