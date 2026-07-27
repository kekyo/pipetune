BUILD_DIR ?= build/release
TEST_BUILD_DIR ?= build/test
BUILD_TYPE ?= Release
PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all test install

all:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" -DBUILD_TESTING=OFF
	cmake --build "$(BUILD_DIR)" --parallel

test:
	cmake -S . -B "$(TEST_BUILD_DIR)" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
	cmake --build "$(TEST_BUILD_DIR)" --parallel
	ctest --test-dir "$(TEST_BUILD_DIR)" --output-on-failure

install: all
	cmake --install "$(BUILD_DIR)" --prefix "$(PREFIX)" --strip
