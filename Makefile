BUILD_DIR ?= build/release
TEST_BUILD_DIR ?= build/test
BUILD_TYPE ?= Release
PREFIX ?= /usr/local
WIREPLUMBER_CONFIG_DIR ?= /usr/share/wireplumber
WIREPLUMBER_DATA_DIR ?= /usr/share/wireplumber
DESTDIR ?=

.PHONY: all test install build-install uninstall

all:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" -DCMAKE_INSTALL_PREFIX="$(PREFIX)" -DPIPETUNE_WIREPLUMBER_CONFIG_DIR="$(WIREPLUMBER_CONFIG_DIR)" -DPIPETUNE_WIREPLUMBER_DATA_DIR="$(WIREPLUMBER_DATA_DIR)" -DBUILD_TESTING=OFF
	cmake --build "$(BUILD_DIR)" --parallel

test:
	cmake -S . -B "$(TEST_BUILD_DIR)" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX="$(PREFIX)" -DPIPETUNE_WIREPLUMBER_CONFIG_DIR="$(WIREPLUMBER_CONFIG_DIR)" -DPIPETUNE_WIREPLUMBER_DATA_DIR="$(WIREPLUMBER_DATA_DIR)" -DBUILD_TESTING=ON
	cmake --build "$(TEST_BUILD_DIR)" --parallel
	ctest --test-dir "$(TEST_BUILD_DIR)" --output-on-failure

install:
	cmake --install "$(BUILD_DIR)" --prefix "$(PREFIX)" --strip

build-install: all
	cmake --install "$(BUILD_DIR)" --prefix "$(PREFIX)" --strip

uninstall:
	@test -f "$(BUILD_DIR)/install_manifest.txt" || { \
		echo "Install manifest not found: $(BUILD_DIR)/install_manifest.txt" >&2; \
		exit 1; \
	}
	@set -e; \
	while IFS= read -r installed_file; do \
		if [ -n "$$installed_file" ]; then \
			printf 'Removing %s\n' "$$installed_file"; \
			rm -f -- "$$installed_file"; \
		fi; \
	done < "$(BUILD_DIR)/install_manifest.txt"
