echo 'cloning hogd'

git init -q .
# Retry the network-facing git steps: github.com drops SSH sessions when the
# hog fleet clones in lockstep. See "Cloning on the hog hosts" in README-ci.md.
hog_git_retry() {
  local attempt
  for attempt in 1 2 3 4 5; do
    "$@" && return 0
    echo "hog_git_retry: [$*] failed (attempt ${attempt}); retrying" >&2
    sleep $(( attempt * 5 + $$ % 7 ))
  done
  "$@"
}
git remote add origin git@github.com:$GIT_REPO.git
hog_git_retry git fetch --depth 1 origin $GIT_COMMIT
git checkout $GIT_COMMIT
hog_git_retry git submodule update --init --recursive
