#!/bin/sh
# Runs as root (the app asks for the admin password).
# Usage: install-queue.sh <filter binary> <ppd> <queue> <device uri> <description>
set -eu
dir=/Library/Printers/JustPrintAlready
mkdir -p "$dir"
cp "$1" "$dir/"
chown root:wheel "$dir/$(basename "$1")"
chmod 755 "$dir/$(basename "$1")"
/usr/sbin/lpadmin -p "$3" -E -v "$4" -P "$2" -D "$5" -o printer-is-shared=false
/usr/sbin/cupsenable "$3"
/usr/sbin/cupsaccept "$3"
