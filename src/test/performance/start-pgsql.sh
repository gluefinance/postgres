#!/bin/sh

# Please choose amount of sort memory (-S XXX) as appropriate
# for your system: more is better, but swapping breaks performance!

exec postmaster -B 256 '-o -S 2048' -S
