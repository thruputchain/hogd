echo 'cloning hogd'

# The clone runs on every host of the job at once, and several hog_sched runs
# can be released together the moment the fleet frees up, so a burst of SSH
# connections reaches GitHub in the same second. GitHub answers such a burst by
# closing connections mid-handshake ("Connection closed by 140.82.116.3 port
# 22"), which fails the clone at second one and discards a run that has already
# spent its wait in the queue. Retry the two steps of the clone that talk to
# GitHub; both are idempotent, so a retry resumes rather than restarts. See
# issue #3649.
git_retry() {
  git_retry_attempt=1
  until "$@"; do
    if [ "$git_retry_attempt" -ge 5 ]; then
      echo "clone step failed after $git_retry_attempt attempts: $*" >&2
      return 1
    fi
    # $$ differs per host and per run, so the jitter spreads hosts that
    # collided on one attempt instead of lining them up again on the next.
    sleep $((git_retry_attempt * 15 + $$ % 15))
    git_retry_attempt=$((git_retry_attempt + 1))
  done
}

git init -q .
git remote add origin git@github.com:$GIT_REPO.git
git_retry git fetch --depth 1 origin $GIT_COMMIT
git checkout $GIT_COMMIT
git_retry git submodule update --init --recursive
