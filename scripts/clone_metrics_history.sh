# Check if metrics_history/ is a git repo; if not, clone it
if [ ! -d "metrics_history/.git" ]; then
  echo "Cloning groundhogchain/metrics_history.git into metrics_history/"
  git clone https://github.com/groundhogchain/metrics_history.git metrics_history/
else
  echo "metrics_history/ already exists and is a git repository."
fi
