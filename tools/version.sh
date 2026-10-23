#!/bin/sh
# Prints the build version, derived from git (same scheme as ../mzm and ../sm-3ds):
#
#   - HEAD exactly on a tag vX.Y.Z:  X.Y.Z
#   - on (or branched off) release/vX.Y.Z:
#                                    X.Y.Z-dev.<commits since main>[.<commits since
#                                    the release branch>]+<hash>
#   - anything else:                 0.0.0-dev+<hash>
#
# Only three-component tags count (vX.Y.Z). The decompilation's own tags (1.0, release, v1.0.0, v1.1.0) live in this
# repository too: none of them sits on our release line, and the release workflow skips them when it looks for the
# previous release.
#
# No leading "v": the updater, the file names and the SMDH all add their own.
# MANIA3DS_VERSION in the environment overrides everything.
set -eu

if [ -n "${MANIA3DS_VERSION:-}" ]; then
  echo "${MANIA3DS_VERSION#v}"
  exit 0
fi

cd "$(dirname "$0")"
OUR_TAGS='v[0-9]*.[0-9]*.[0-9]*'

if ! git rev-parse --git-dir >/dev/null 2>&1; then
  echo "0.0.0-dev"
  exit 0
fi

is_ours() {
  # The glob above also matches v1.2.3-rc and similar; keep plain X.Y.Z only.
  echo "$1" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$'
}

EXACT=$(git tag --points-at HEAD --list "$OUR_TAGS" 2>/dev/null | sort -V | tail -1)
if [ -n "$EXACT" ] && is_ours "$EXACT"; then
  echo "${EXACT#v}"
  exit 0
fi

HASH=$(git rev-parse --short HEAD 2>/dev/null || echo dev)
BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo HEAD)

case "$BRANCH" in
  release/*) REL="$BRANCH" ;;
  *)
    # Topic branches cut from a release branch inherit its version: the newest
    # release/* branch (local or origin) that HEAD contains.
    REL=$(git for-each-ref --format='%(refname:short)' --merged HEAD \
            'refs/heads/release/*' 'refs/remotes/origin/release/*' 2>/dev/null |
          sed 's#^origin/##' | sort -V | tail -1)
    ;;
esac

case "$REL" in
  release/v*) ;;
  *)
    echo "0.0.0-dev+$HASH"
    exit 0
    ;;
esac

if git rev-parse --verify -q "refs/heads/$REL" >/dev/null; then
  TARGET="refs/heads/$REL"
else
  TARGET="refs/remotes/origin/$REL"
fi
if git rev-parse --verify -q refs/remotes/origin/main >/dev/null; then
  BASE=refs/remotes/origin/main
else
  BASE=main
fi
REL_CNT=$(git rev-list --count "$BASE..$TARGET" 2>/dev/null || echo 1)
DEV_CNT=$(git rev-list --count "$TARGET..HEAD" 2>/dev/null || echo 0)
NUM=${REL#release/v}

if [ "$DEV_CNT" = 0 ]; then
  echo "$NUM-dev.$REL_CNT+$HASH"
else
  echo "$NUM-dev.$REL_CNT.$DEV_CNT+$HASH"
fi
