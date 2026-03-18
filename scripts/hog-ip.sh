# Returns the IP address for a given hog machine hostname.
# Example: if "hog12" is the argument, returns "10.100.0.112"
hog_ip() {
  local host=$1
  local hog_num=${host#hog}
  if [[ $host == "$hog_num" || ! $hog_num =~ ^[0-9]+$ ]]; then
    echo "invalid hog host: $host" >&2
    return 1
  fi
  echo "10.100.0.$((100 + hog_num))"
}
