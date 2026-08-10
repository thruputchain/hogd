#!/usr/bin/env bash

# Names the host that is going to break a hog_sched run, before the run starts.
#
# Usage: check_bench_hosts.sh <schedule.toml>
#
# Every job hog_sched starts is preceded by a prologue it injects itself:
#
#     export HOME="/tmp/hogjob-hogci"
#     echo "forwarding github urls through ssh-agent"
#     hog_fwd
#
# [hog_fwd] maps each private groundhogchain repo to an identity held by the
# ssh-agent this run forwarded into the host, and the prologue runs under
# [set -euo pipefail]. A host whose sshd hands the session no forwarded agent
# therefore fails the job on its second line, with
#
#     Could not open a connection to your authentication agent.
#
# and takes the whole schedule down with it. That is a host fault -- nothing in
# this repo can repair it -- but the way it reports is a problem of its own:
#
#   - the line above is one line among the ~800 that eight hosts emit while
#     forwarding nineteen keys each, so it does not stand out;
#   - the schedule's own after-script is preceded by the same prologue and dies
#     the same way, so "after-script on <host> failed" is the *last* error in
#     the job log and reads like the failure, burying the first one;
#   - the issue [.github/actions/file-failure-issue] files says only that the
#     build job failed, and GitHub serves only a bounded tail of these logs.
#
# So check the hosts a schedule is about to lock while there is still a cheap
# place to say something, and name the ones that cannot run it.
#
# Only [no-agent] fails this check, because that is the one condition observed
# to be fatal to a run. A host that cannot be reached at all is reported and
# tolerated -- the fault may be in this check's own ssh invocation rather than
# in the host, and hog_sched is better placed to report a host that is really
# down -- and so is an agent that holds no identities, which hog_fwd may yet
# survive. The check can therefore turn a cryptic failure into a named one, but
# not create a failure of its own.

set -uo pipefail

CHECK_SSH_OPTS=(-A -n -T -o BatchMode=yes -o ConnectTimeout=10 -o VisualHostKey=no)

# A single failed session is not a broken host, so no host is called bad until
# every attempt has failed to find it a usable agent.
CHECK_ATTEMPTS=${CHECK_BENCH_HOSTS_ATTEMPTS:-2}
CHECK_RETRY_DELAY_SEC=${CHECK_BENCH_HOSTS_RETRY_DELAY_SEC:-2}

# Overridable so the self-test can drive the classification without a network.
CHECK_SSH=${CHECK_BENCH_HOSTS_SSH:-ssh}
CHECK_SSH_ADD=${CHECK_BENCH_HOSTS_SSH_ADD:-ssh-add}

usage() {
  echo "usage: check_bench_hosts.sh <schedule.toml>" >&2
  exit 2
}

# The hosts a schedule locks are the values of its [jobs.host] tables, e.g.
#
#     [jobs.host]
#     bench_1 = "hog6"
#
# Read them from the schedule rather than restating them in the workflow, so
# the check cannot drift from the run it is checking. A value of "?" asks
# hog_sched for whichever host is free, which is not known until the run
# starts; those are counted separately and left alone.
schedule_hosts() {
  local schedule=$1

  awk '
    /^[[:space:]]*\[/ {
      in_hosts = ($0 ~ /^[[:space:]]*\[jobs\.host\][[:space:]]*$/)
      next
    }
    !in_hosts { next }
    /^[[:space:]]*#/ { next }
    match($0, /"[^"]+"/) {
      host = substr($0, RSTART + 1, RLENGTH - 2)
      if (host != "?") { print host }
    }
  ' "$schedule" | sort -u
}

# How many hosts the schedule leaves for hog_sched to choose.
schedule_wildcard_count() {
  local schedule=$1

  awk '
    /^[[:space:]]*\[/ {
      in_hosts = ($0 ~ /^[[:space:]]*\[jobs\.host\][[:space:]]*$/)
      next
    }
    !in_hosts { next }
    /^[[:space:]]*#/ { next }
    match($0, /"[^"]+"/) {
      if (substr($0, RSTART + 1, RLENGTH - 2) == "?") { count++ }
    }
    END { print count + 0 }
  ' "$schedule"
}

# Whether the forwarded agent on $1 can be used, as one of [ok], [no-agent],
# [no-identities] or [unreachable].
#
# ssh reports the remote command's status and reserves 255 for failures of its
# own. [ssh-add -l] exits 0 with identities, 1 when it reached an agent that
# holds none, and 2 when it could not reach an agent at all -- the case that
# kills hog_fwd.
#
# Only [ok] ends the attempts. Every other verdict stays provisional until they
# are all spent, so a host is called bad only if no attempt ever found it a
# usable agent -- including the [no-agent] verdict, which is the one that fails
# the run and so is the one that most needs a second opinion. The verdict
# reported is the last one seen.
host_agent_state() {
  local host=$1
  local attempt status state=unreachable

  for ((attempt = 1; attempt <= CHECK_ATTEMPTS; attempt++)); do
    [ "$attempt" -eq 1 ] || sleep "$CHECK_RETRY_DELAY_SEC"

    "$CHECK_SSH" "${CHECK_SSH_OPTS[@]}" "$host" 'ssh-add -l' >/dev/null 2>&1
    status=$?

    case $status in
    0)
      echo ok
      return 0
      ;;
    1) state=no-identities ;;
    2) state=no-agent ;;
    *) state=unreachable ;;
    esac
  done

  echo "$state"
}

describe_state() {
  case $1 in
  no-agent)
    echo 'sshd gave the session no forwarded ssh-agent, so hog_fwd fails with' \
      '"Could not open a connection to your authentication agent."'
    ;;
  no-identities)
    echo 'the forwarded ssh-agent holds no identities (tolerated: hog_fwd may' \
      'still find what it needs)'
    ;;
  unreachable)
    echo 'could not be reached by this check (tolerated: hog_sched reports a' \
      'host that is genuinely down)'
    ;;
  *) echo "reported nothing at all (tolerated)" ;;
  esac
}

main() {
  [ $# -eq 1 ] || usage
  local schedule=$1
  local -a hosts=() broken=() tolerated=()
  local host state state_dir index agent_status wildcards

  if [ ! -f "$schedule" ]; then
    echo "check_bench_hosts: no such schedule: $schedule" >&2
    exit 2
  fi

  if ! command -v "$CHECK_SSH" >/dev/null 2>&1; then
    echo "check_bench_hosts: no $CHECK_SSH on PATH" >&2
    exit 2
  fi

  mapfile -t hosts < <(schedule_hosts "$schedule")
  wildcards=$(schedule_wildcard_count "$schedule")
  if [ "${#hosts[@]}" -eq 0 ]; then
    if [ "$wildcards" -gt 0 ]; then
      echo "check_bench_hosts: $(basename "$schedule") leaves all" \
        "$wildcards of its hosts for hog_sched to choose; nothing to check"
      exit 0
    fi
    echo "check_bench_hosts: $schedule names no hosts to check" >&2
    exit 2
  fi

  # A runner whose own agent is gone would report every host as broken, which
  # says nothing about the hosts. Name that case for what it is instead.
  "$CHECK_SSH_ADD" -l >/dev/null 2>&1
  agent_status=$?
  if [ "$agent_status" -eq 2 ]; then
    echo 'check_bench_hosts: this runner has no reachable ssh-agent of its' \
      'own, so there is nothing to forward to the bench hosts' >&2
    exit 1
  fi

  # Eight hosts each waiting out a connect timeout is worth avoiding, so look
  # at them all at once and collect the verdicts from the filesystem. The files
  # are named by index: a host name is schedule data, not a safe path.
  state_dir=$(mktemp -d) || exit 2
  # shellcheck disable=SC2064 # Expand $state_dir now, while it is still set.
  trap "rm -rf '$state_dir'" EXIT

  for index in "${!hosts[@]}"; do
    host_agent_state "${hosts[index]}" >"$state_dir/$index" &
  done
  wait

  for index in "${!hosts[@]}"; do
    host=${hosts[index]}
    state=$(cat "$state_dir/$index" 2>/dev/null)
    case $state in
    ok) ;;
    no-agent)
      broken+=("$host")
      echo "bench host $host: $(describe_state "$state")" >&2
      ;;
    *)
      tolerated+=("$host")
      echo "bench host $host: $(describe_state "$state")" >&2
      ;;
    esac
  done

  if [ "${#broken[@]}" -ne 0 ]; then
    echo "check_bench_hosts: ${#broken[@]} of ${#hosts[@]} hosts cannot run" \
      "$(basename "$schedule"): ${broken[*]}" >&2
    echo 'check_bench_hosts: this is a host fault rather than a defect in the' \
      'commit under test; the host needs an operator before this schedule can' \
      'run' >&2
    exit 1
  fi

  echo "check_bench_hosts: ${#hosts[@]} hosts ready for" \
    "$(basename "$schedule") (${hosts[*]})"
  if [ "$wildcards" -gt 0 ]; then
    echo "check_bench_hosts: $wildcards further host(s) are chosen by" \
      "hog_sched at run time and were not checked"
  fi
  if [ "${#tolerated[@]}" -ne 0 ]; then
    echo "check_bench_hosts: continuing despite ${#tolerated[@]} host(s) this" \
      "check could not clear: ${tolerated[*]}"
  fi
}

main "$@"
