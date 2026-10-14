#!/bin/sh

set -ex

killall hogx-agent
pkill -f '^/tmp/hogx/pkg'
