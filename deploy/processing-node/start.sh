#!/bin/sh
set -eu
if [ ! -s /run/uam-authorized-keys ]; then
    echo 'Mount a nonempty public authorized_keys file at /run/uam-authorized-keys.' >&2
    exit 1
fi
ssh-keygen -l -f /run/uam-authorized-keys >/dev/null
install -d -m 700 -o uam -g uam /home/uam/.ssh
install -m 600 -o uam -g uam /run/uam-authorized-keys /home/uam/.ssh/authorized_keys
if [ ! -f /var/lib/uam-host-keys/ssh_host_ed25519_key ]; then
    ssh-keygen -q -t ed25519 -N '' -f /var/lib/uam-host-keys/ssh_host_ed25519_key
fi
exec /usr/sbin/sshd -D -e
