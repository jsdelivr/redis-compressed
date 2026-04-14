.PHONY: debug release test test-debug clean

DEBUG_BUILD_DIR := build/debug
RELEASE_BUILD_DIR := build/release
TEST_FILE := test/tests/integration.js
DEBUG_MODULE := $(abspath $(DEBUG_BUILD_DIR)/libredis-compressed.so)
RELEASE_MODULE := $(abspath $(RELEASE_BUILD_DIR)/libredis-compressed.so)

debug:
	cmake -S . -B $(DEBUG_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(DEBUG_BUILD_DIR) --parallel -- --no-print-directory

release:
	cmake -S . -B $(RELEASE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(RELEASE_BUILD_DIR) --parallel -- --no-print-directory

test: release
	REDIS_COMPRESSED_MODULE=$(RELEASE_MODULE) node --test $(TEST_FILE)

test-debug: debug
	REDIS_COMPRESSED_MODULE=$(DEBUG_MODULE) node --test $(TEST_FILE)

clean:
	rm -rf build
