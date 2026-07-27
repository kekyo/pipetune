#!/bin/sh

set -eu

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

require_env() {
	var_name=$1
	eval "var_value=\${$var_name:-}"
	[ -n "$var_value" ] ||
		fail "Missing required environment variable: $var_name"
}

require_command() {
	command -v "$1" >/dev/null 2>&1 ||
		fail "Missing required command: $1"
}

validate_positive_integer() {
	value_name=$1
	value=$2

	case $value in
		'' | *[!0-9]*)
			fail "$value_name must be a positive integer: $value"
			;;
	esac

	[ "$value" -gt 0 ] ||
		fail "$value_name must be a positive integer: $value"
}

assert_file() {
	[ -f "$1" ] || fail "Missing expected file: $1"
}

copy_docs() {
	stage_dir=$1
	doc_dir="$stage_dir/usr/share/doc/$PIPETUNE_PACKAGE_NAME"

	mkdir -p "$doc_dir"
	cp README.md "$doc_dir/README.md"
	cp pipetune/README.md "$doc_dir/README.daemon.md"
	cp pipetune-gtk/README.md "$doc_dir/README.gtk.md"
	cp pipetune/docs/architecture.md "$doc_dir/architecture.md"
	cp packaging/copyright "$doc_dir/copyright"
}

write_control_file() {
	control_path=$1
	depends_value=$2

	{
		printf 'Package: %s\n' "$PIPETUNE_PACKAGE_NAME"
		printf 'Version: %s\n' "$PIPETUNE_PACKAGE_VERSION"
		printf 'Section: sound\n'
		printf 'Priority: optional\n'
		printf 'Architecture: %s\n' "$deb_arch"
		printf 'Maintainer: %s\n' "$PIPETUNE_PACKAGE_MAINTAINER"
		printf 'Depends: %s\n' "$depends_value"
		printf 'Homepage: https://github.com/kekyo/pipetune\n'
		printf 'Description: %s\n' "$PIPETUNE_PACKAGE_DESCRIPTION"
	} >"$control_path"
}

calculate_shlibdeps() {
	tmp_dir=$(mktemp -d)

	mkdir -p "$tmp_dir/debian"
	{
		printf 'Source: pipetune\n'
		printf 'Section: sound\n'
		printf 'Priority: optional\n'
		printf 'Maintainer: %s\n' "$PIPETUNE_PACKAGE_MAINTAINER"
		printf 'Standards-Version: 4.6.2\n'
		printf '\n'
		printf 'Package: %s\n' "$PIPETUNE_PACKAGE_NAME"
		printf 'Architecture: %s\n' "$deb_arch"
		printf 'Description: temporary package metadata for dependency calculation\n'
	} >"$tmp_dir/debian/control"

	depends_value=$(
		cd "$tmp_dir"
		dpkg-shlibdeps \
			-O \
			"$build_dir/pipetune" \
			"$build_dir/pipetune-gtk" |
			sed -n 's/^shlibs:Depends=//p'
	)
	rm -rf "$tmp_dir"

	[ -n "$depends_value" ] ||
		fail 'dpkg-shlibdeps did not calculate runtime dependencies'
	printf '%s\n' "$depends_value"
}

validate_installed_package() {
	package_path=$1

	require_env PIPETUNE_PACKAGE_VERSION
	require_env PIPETUNE_PACKAGE_NAME
	require_command dpkg
	require_command dpkg-query
	assert_file "$package_path"

	dpkg -i "$package_path"
	installed_status=$(dpkg-query -W -f='${Status}' "$PIPETUNE_PACKAGE_NAME")
	[ "$installed_status" = 'install ok installed' ] ||
		fail "Package installation did not complete: $installed_status"

	for installed_file in \
		/usr/bin/pipetune \
		/usr/bin/pipetune-gtk \
		/usr/lib/systemd/user/pipetune.service \
		/usr/share/applications/net.kekyo.pipetune-gtk.desktop \
		/etc/xdg/autostart/net.kekyo.pipetune-gtk.desktop \
		/usr/share/icons/hicolor/scalable/apps/pipetune.svg \
		/usr/share/doc/pipetune/copyright; do
		assert_file "$installed_file"
	done

	pipetune_version=$(/usr/bin/pipetune --version)
	[ "$pipetune_version" = "pipetune $PIPETUNE_PACKAGE_VERSION" ] ||
		fail "Unexpected pipetune version: $pipetune_version"
	gtk_version=$(/usr/bin/pipetune-gtk --version)
	[ "$gtk_version" = "pipetune-gtk $PIPETUNE_PACKAGE_VERSION" ] ||
		fail "Unexpected pipetune-gtk version: $gtk_version"

	printf '%s\n' "Installed package validated: $package_path"
}

if [ "${1:-}" = '--validate-package' ]; then
	[ "$#" -eq 2 ] || fail 'Usage: build_linux_dist_container.sh --validate-package <path>'
	validate_installed_package "$2"
	exit 0
fi

[ "$#" -eq 0 ] || fail "Unknown argument: $1"

require_env PIPETUNE_WORK_DIR
require_env PIPETUNE_PACKAGE_VERSION
require_env PIPETUNE_PACKAGE_NAME
require_env PIPETUNE_PACKAGE_DESCRIPTION
require_env PIPETUNE_PACKAGE_MAINTAINER
require_env PIPETUNE_BUILD_TYPE

PIPETUNE_MAKE_JOBS=${PIPETUNE_MAKE_JOBS:-1}
validate_positive_integer 'PIPETUNE_MAKE_JOBS' "$PIPETUNE_MAKE_JOBS"

work_dir=$PIPETUNE_WORK_DIR
build_dir="$work_dir/build"
stage_dir="$work_dir/stage/$PIPETUNE_PACKAGE_NAME"

rm -rf "$work_dir"
mkdir -p "$build_dir"

require_command cmake
require_command dpkg-architecture
require_command dpkg-shlibdeps
require_command pkg-config
require_command node

pkg-config --exists gtk+-3.0 ||
	fail 'Missing required pkg-config module: gtk+-3.0'
pkg-config --exists libpipewire-0.3 ||
	fail 'Missing required pkg-config module: libpipewire-0.3'

cmake \
	-S . \
	-B "$build_dir" \
	-DCMAKE_BUILD_TYPE="$PIPETUNE_BUILD_TYPE" \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DBUILD_TESTING=OFF \
	-DPIPETUNE_BUILD_VERSION="$PIPETUNE_PACKAGE_VERSION"
cmake --build "$build_dir" --parallel "$PIPETUNE_MAKE_JOBS"

assert_file "$build_dir/pipetune"
assert_file "$build_dir/pipetune-gtk"

rm -rf "$stage_dir"
mkdir -p "$stage_dir"
if [ "$PIPETUNE_BUILD_TYPE" = 'Release' ]; then
	DESTDIR="$stage_dir" cmake --install "$build_dir" --prefix /usr --strip
else
	DESTDIR="$stage_dir" cmake --install "$build_dir" --prefix /usr
fi
copy_docs "$stage_dir"

deb_arch=$(dpkg-architecture -qDEB_HOST_ARCH)
shlib_depends=$(calculate_shlibdeps)
runtime_depends='systemd, dbus-user-session, pipewire, wireplumber, hicolor-icon-theme'
control_dir="$stage_dir/DEBIAN"
mkdir -p "$control_dir"
write_control_file "$control_dir/control" "$shlib_depends, $runtime_depends"
chmod 0644 "$control_dir/control"

assert_file "$stage_dir/usr/bin/pipetune"
assert_file "$stage_dir/usr/bin/pipetune-gtk"
assert_file "$stage_dir/usr/lib/systemd/user/pipetune.service"
assert_file "$stage_dir/usr/share/applications/net.kekyo.pipetune-gtk.desktop"
assert_file "$stage_dir/etc/xdg/autostart/net.kekyo.pipetune-gtk.desktop"
assert_file "$stage_dir/usr/share/icons/hicolor/scalable/apps/pipetune.svg"
assert_file "$stage_dir/usr/share/doc/pipetune/copyright"
