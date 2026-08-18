#!/bin/sh
# Add the Aqualink Automate APT repository and install (Debian / Ubuntu / Raspberry
# Pi OS). After this, `sudo apt upgrade` keeps it up to date.
#
#   curl -fsSL https://iainchesworthlabs.github.io/aqualink-automate/install-apt.sh | sh
set -e

BASE="https://iainchesworthlabs.github.io/aqualink-automate"
SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO="sudo"

command -v curl >/dev/null 2>&1 || { echo "Please install curl first." >&2; exit 1; }

$SUDO install -d -m 0755 /etc/apt/keyrings
curl -fsSL "$BASE/key.gpg" | $SUDO gpg --dearmor -o /etc/apt/keyrings/aqualink-automate.gpg
echo "deb [signed-by=/etc/apt/keyrings/aqualink-automate.gpg] $BASE/apt stable main" \
  | $SUDO tee /etc/apt/sources.list.d/aqualink-automate.list >/dev/null

$SUDO apt-get update
$SUDO apt-get install -y aqualink-automate

echo
echo "Installed. Next: set your serial port in /etc/aqualink-automate/aqualink-automate.conf"
echo "then: sudo systemctl start aqualink-automate"
