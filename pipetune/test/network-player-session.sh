#!/bin/sh
# Run the optional receiver and transient user-service tests on a private LAN.
set -eu
if [ "${1:-}" = "--enter" ]; then
    shift
    owner=$1
    shift
    exec podman unshare nsenter "--net=/proc/$owner/ns/net" \
        unshare --user "--map-user=$(id -u)" "--map-group=$(id -g)" "$@"
fi
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 BUILD_DIRECTORY EFFETUNE_SOURCE_DIRECTORY" >&2
    exit 2
fi
script=$(realpath "$0")
export PIPETUNE_TEST_UNIT_WRAPPER="$script"
export PIPETUNE_TEST_MANAGER_RUNTIME="${XDG_RUNTIME_DIR:?}"
exec podman unshare unshare --net sh -ec '
    ip link set lo up
    ip link add pt3 address 02:10:20:30:40:50 type dummy
    ip addr add 192.168.79.1/24 dev pt3
    ip link set pt3 up
    ip route add 224.0.0.0/4 dev pt3
    exec unshare --user "--map-user=$1" "--map-group=$2" dbus-run-session -- \
        "$3" "$4" "$5" pt3 "$6"
' sh "$(id -u)" "$(id -g)" "$(command -v node)" "$(dirname "$script")/network-player.mjs" "$1" "$2"
