echo 'cloning hogd'

git init -q .
git remote add origin git@github.com:$GIT_REPO.git
git fetch --depth 1 origin $GIT_COMMIT
git checkout $GIT_COMMIT
git submodule update --init --recursive
