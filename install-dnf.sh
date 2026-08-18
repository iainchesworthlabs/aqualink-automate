#!/bin/sh
# Add the Aqualink Automate DNF/YUM repository and install (Fedora / RHEL / openSUSE).
# After this, `sudo dnf upgrade` keeps it up to date.
#
#   curl -fsSL https://iainchesworthlabs.github.io/aqualink-automate/install-dnf.sh | sh
set -e

BASE="https://iainchesworthlabs.github.io/aqualink-automate"
SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO="sudo"

PM="dnf"; command -v dnf >/dev/null 2>&1 || PM="yum"

$SUDO rpm --import "$BASE/key.gpg"
$SUDO tee /etc/yum.repos.d/aqualink-automate.repo >/dev/null <<EOF
[aqualink-automate]
name=Aqualink Automate
baseurl=$BASE/rpm
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$BASE/key.gpg
EOF

$SUDO "$PM" install -y aqualink-automate

echo
echo "Installed. Next: set your serial port in /etc/aqualink-automate/aqualink-automate.conf"
echo "then: sudo systemctl start aqualink-automate"
