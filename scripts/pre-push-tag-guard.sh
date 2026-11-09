#!/bin/sh
# pre-push-tag-guard.sh - keep the local historical tag v0.2.4 off the remote.
#
# v0.2.4 is a local annotated tag on 1b64df0 (tbtun 0.2.4); origin does not
# have it, and the release policy keeps it that way. `git push --tags` would
# create it by accident. This script refuses that push.
#
# Two ways to use it:
#
#   1. Manual release check:
#        scripts/pre-push-tag-guard.sh [remote]
#      Lists every local tag that the remote does not have, i.e. every tag
#      that `git push --tags` would create, and exits 2 when v0.2.4 is among
#      them. The remote query is read-only: `git ls-remote --tags`.
#
#   2. Optional pre-push hook (works in linked worktrees):
#        hook=$(git rev-parse --git-path hooks/pre-push)
#        mkdir -p "$(dirname "$hook")"
#        cat > "$hook" <<'EOF'
#        #!/bin/sh
#        root=$(git rev-parse --show-toplevel) || exit 1
#        exec "$root/scripts/pre-push-tag-guard.sh" --hook
#        EOF
#        chmod +x "$hook"
#      Git then feeds the refs it is about to push on stdin. The hook rejects
#      the push when refs/tags/v0.2.4 would be created.
#
# Neither mode deletes, moves or rewrites a tag. When the remote query fails,
# manual mode exits 3 and tells the operator to avoid `--tags`.
set -eu

TAG=v0.2.4
MODE=manual
REMOTE=origin

say() { printf '%s\n' "$*"; }

usage() {
    cat <<EOF
usage: ${0##*/} [remote]      list tags that 'git push --tags' would create
       ${0##*/} --hook        read refs from stdin (git pre-push hook)

Refuses the push when the local historical tag $TAG would be created on the
remote. Never deletes or moves a tag. Exit: 0 clean, 2 refused, 3 remote error.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --hook) MODE=hook ;;
        -h | --help) usage; exit 0 ;;
        -*) say "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
        *) REMOTE=$arg ;;
    esac
done

if [ "$MODE" = hook ]; then
    status=0
    while read -r local_ref _local_sha _remote_ref remote_sha; do
        [ "$local_ref" = "refs/tags/$TAG" ] || continue
        # All-zero remote sha means "create this ref"; a non-zero sha means it
        # already exists, which this policy also treats as unexpected.
        case "$remote_sha" in
            *[!0]*) ;;
            *)
                say "pre-push-tag-guard: refusing to create $TAG on the remote." >&2
                say "  $TAG is local history; the release policy keeps it unpushed." >&2
                say "  Push the intended release tag explicitly:" >&2
                say "    git push <remote> refs/tags/<release-tag>" >&2
                say "  Do not use: git push --tags" >&2
                status=2
                ;;
        esac
    done || true
    exit "$status"
fi

# Manual mode: ask the remote once, then compare local tags with it.
TMP=$(mktemp "${TMPDIR:-/tmp}/tbt-tags.XXXXXX")
trap 'rm -f "$TMP"' EXIT HUP INT TERM
if ! git ls-remote --tags "$REMOTE" >"$TMP" 2>/dev/null; then
    say "ERROR: cannot read tags from '$REMOTE'." >&2
    say "Do not run 'git push --tags' while the remote state is unknown." >&2
    say "Push the release tag explicitly instead." >&2
    exit 3
fi

say "tags that 'git push $REMOTE --tags' would create:"
blocked=0
any=0
for t in $(git for-each-ref --format='%(refname:short)' refs/tags); do
    if grep -qxF "refs/tags/$t" "$TMP" || grep -qxF "refs/tags/$t^{}" "$TMP"; then
        continue
    fi
    any=1
    say "  $t"
    if [ "$t" = "$TAG" ]; then
        blocked=1
    fi
done
if [ "$any" -eq 0 ]; then
    say "  (none)"
fi

if [ "$blocked" -eq 1 ]; then
    say "" >&2
    say "REFUSED: $TAG would be created on '$REMOTE'." >&2
    say "$TAG is a local historical tag; the release policy keeps it unpushed." >&2
    say "Push the intended release tag explicitly:" >&2
    say "  git push $REMOTE refs/tags/<release-tag>" >&2
    say "Do not use: git push --tags" >&2
    exit 2
fi

say ""
say "OK: $TAG would not be pushed."
