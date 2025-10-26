if [[ ! -v COMMON_SSH_OPTS ]]; then
  COMMON_SSH_OPTS=(
    -n -T -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no
  )
fi

STDIN_SSH_OPTS=()
for x in "${COMMON_SSH_OPTS[@]}"; do
  [[ $x != -n ]] && STDIN_SSH_OPTS+=("$x")
done

: ${EXPORT_REGEX:=$^}
rm -f remote-exec.env
export -p | sed -e 's/^declare -x //' | grep -e "$EXPORT_REGEX" || : |
  sed -e 's/^/export /' >remote-exec.env

# remote_exec [user@]host /local/exe [--] [args...]
remote_exec() {
  local remote="$1" exe="$2"
  shift 2
  [[ "$1" == "--" ]] && shift
  [[ -x "$exe" ]] || {
    echo "$exe: no such executable" >&2
    return 2
  }

  local base="$(basename -- "$exe")" dir="$(dirname -- "$exe")"

  tar -cf - remote-exec.env -C "$dir" "$base" |
    ssh "${STDIN_SSH_OPTS[@]}" -- "$remote" '
set -euo pipefail
workdir="/tmp/${USER:-$(whoami)}"
mkdir -p "$workdir"
cd "$workdir"
if ! [[ -O . && ! -h "$workdir" ]]; then
   echo "$workdir owned by another user, failing" >&2
   exit 1
fi

base="'"$base"'"
tar xf -
ulimit -c unlimited || :
. remote-exec.env
exec "./$base" '"$@"'
'
}
