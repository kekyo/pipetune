BUILD_DIR ?= ../../build/asset-preparation

.PHONY: all test
all:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
	cmake --build "$(BUILD_DIR)" --parallel

test: all
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure
