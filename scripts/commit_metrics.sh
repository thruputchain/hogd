# Push metrics_history/ to groundhogchain/metrics_history.git
cd metrics_history/

# Check if git committer name and email are set; if not, set them to metrics-script
committer_name=$(git config user.name || true)
committer_email=$(git config user.email || true)
if [ -z "$committer_name" ]; then
  committer_name="metrics-script"
fi
if [ -z "$committer_email" ]; then
  committer_email="metrics-script@groundhogchain.com"
fi
echo "Committer name: $committer_name"
echo "Committer email: $committer_email"

git add metrics/
GIT_COMMITTER_NAME="$committer_name" \
  GIT_COMMITTER_EMAIL="$committer_email" \
  git commit -m "Add metrics for $branch $commit" --author "$committer_name <$committer_email>"
git push

