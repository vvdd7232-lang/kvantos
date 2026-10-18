#!/usr/bin/env bash
# Authorized user action: push the prepared 3.1.4 refs; CI validates/publishes.
# No tokens are accepted as arguments or stored by this script.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=vvdd7232-lang/kvantos
url=https://github.com/$repo.git
tag=v3.1.4
expected_main=95c38674b3f22182317fe307214677c239a206e8
command -v gh >/dev/null || { echo 'Install GitHub CLI and run gh auth login first.' >&2; exit 1; }
gh auth status --hostname github.com >/dev/null
permission=$(gh repo view "$repo" --json viewerPermission --jq .viewerPermission)
case "$permission" in ADMIN|MAINTAIN|WRITE) ;; *) echo 'GitHub write permission required.' >&2; exit 1;; esac
commit=$(git rev-parse "$tag^{commit}")
[[ $(git rev-parse HEAD) == "$commit" ]] || { echo "Check out $tag first." >&2; exit 1; }
[[ -z $(git status --porcelain --untracked-files=no) ]] || { echo 'Tracked changes must be committed first.' >&2; exit 1; }
remote_main=$(git ls-remote "$url" refs/heads/main | cut -f1)
[[ "$remote_main" == "$expected_main" ]] || { echo 'Remote main changed; review before publishing. No refs pushed.' >&2; exit 1; }
[[ -z $(git ls-remote "$url" "refs/tags/$tag") ]] || { echo 'Remote tag already exists; refusing to replace it.' >&2; exit 1; }
# Verify repository/API access separately: a failed network check must not mean absent release.
gh api "repos/$repo/releases?per_page=100" --jq '.[].tag_name' > /dev/null
if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
    echo 'Release already exists; refusing to replace it.' >&2; exit 1
fi
git merge-base --is-ancestor "$expected_main" "$commit"
gh auth setup-git --hostname github.com
git push --atomic "$url" "$commit:refs/heads/main" \
    "$commit:refs/heads/release/$tag" "refs/tags/$tag:refs/tags/$tag"
printf 'Refs pushed. CI will test and publish; monitor https://github.com/%s/actions\n' "$repo"
