# Returns the IP address for a given hog machine hostname.
# Example: if "hog12" is the argument, returns "10.100.0.112"
hog_ip() {
  local host=$1
  local hog_num=${host#hog}
  if [[ $host == "$hog_num" || ! $hog_num =~ ^[0-9]+$ ]]; then
    echo "invalid hog host: $host" >&2
    return 1
  fi
  if [[ "${HOGD_CI_CONTAINER:-0}" == 1 ]]; then
    # Docker's run-local DNS resolves only peers on this run's overlay.
    local address
    address=$(getent ahostsv4 "$host" | awk 'NR == 1 {print $1}')
    [[ -n "$address" ]] || return 1
    printf '%s\n' "$address"
    return
  fi
  echo "10.100.0.$((100 + hog_num))"
}
