# lossylab — project Makefile
# ------------------------------------------------
# Configuration — override on the command line or via environment variables.
BUILD_DIR     := build
BUILD_TYPE    ?= Release

# ------------------------------------------------
# Phony targets
.PHONY: build test clean

# ---------- Build ---------------------------------------------------

build:
	cmake -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		$(CMAKE_EXTRA_FLAGS)
	cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) -j

# ---------- Test ----------------------------------------------------

## Run C++ tests via CTest.
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# ---------- Clean ---------------------------------------------------

## Remove build artifacts.
clean:
	rm -rf $(BUILD_DIR)
