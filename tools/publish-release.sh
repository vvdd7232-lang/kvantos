#!/usr/bin/env bash
# Explicit user action: push the current Arena branch, freeze its tag, and
# dispatch the gated build/test/package/publish workflow. Never move main,
# another branch, an existing tag, or an existing release.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=vvdd7232-lang/kvantos
version=$(awk '$2 == "KV_VERSION" {gsub(/"/, "", $3); print $3}' include/kernel.h)
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid kernel version' >&2; exit 1; }
tag=v$version
branch=$(git branch --show-current)
case "$branch" in arena/*) ;; *) echo 'Run from the active Arena branch, not main or a detached tag.' >&2; exit 1;; esac
[[ -z $(git status --porcelain) ]] || { echo 'Commit all source changes first.' >&2; exit 1; }
commit=$(git rev-parse HEAD)
command -v gh >/dev/null || { echo 'GitHub CLI is required.' >&2; exit 1; }
gh auth status --hostname github.com >/dev/null
permission=$(gh repo view "$repo" --json viewerPermission --jq .viewerPermission)
case "$permission" in ADMIN|MAINTAIN|WRITE) ;; *) echo 'GitHub write permission required.' >&2; exit 1;; esac
# A failed API call is fatal, never interpreted as an absent release/tag.
releases=$(gh api --paginate "repos/$repo/releases?per_page=100" --jq '.[].tag_name')
if grep -Fxq "$tag" <<< "$releases"; then
    echo "Release $tag already exists; refusing to replace it." >&2; exit 1
fi
remote_tag=$(gh api "repos/$repo/git/matching-refs/tags/$tag" --jq ".[] | select(.ref == \"refs/tags/$tag\") | .object.sha")
if [[ -n "$remote_tag" && "$remote_tag" != "$commit" ]]; then
    echo "Tag $tag already points elsewhere; refusing to move it." >&2; exit 1
fi
git push origin "$branch"
if [[ -z "$remote_tag" ]]; then
    gh api --method POST "repos/$repo/git/refs" -f "ref=refs/tags/$tag" -f "sha=$commit" --jq .ref
fi
# A tag freezes sources; publication happens only after the full CI suite.
# Re-dispatching after a failed build is permitted only at that same SHA.
gh workflow run release.yml --repo "$repo" --ref "$branch" --field "tag=$tag"
printf '%s build dispatched from %s (%s). No release is published until tests pass.\n' "$tag" "$branch" "$commit"
