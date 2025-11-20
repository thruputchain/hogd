echo 'cloning hogd'

git clone \
  --separate-git-dir=.git \
  --depth=1 --recursive \
  --branch {{ .git_branch }} \
  git@github.com:groundhogchain/hogd.git tmp
rm -rf tmp
git checkout -q {{ .git_branch }}
git reset --hard
