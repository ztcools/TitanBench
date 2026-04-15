SHELL := /bin/bash

BUILD_DIR ?= build
BUILD_TYPE ?= Release
ARCH ?= $(shell uname -m)
TRIPLET ?= $(if $(filter aarch64 arm64,$(ARCH)),arm64-linux,x64-linux)

CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DVCPKG_TARGET_TRIPLET=$(TRIPLET)

.PHONY: all configure build clean rebuild run package

all: build

configure:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: configure
	cmake --build $(BUILD_DIR) -j$$(nproc)

rebuild: clean build

run: build
	./$(BUILD_DIR)/titanbench --help

package: build
	strip "$(BUILD_DIR)/titanbench" || true
	mkdir -p dist
	cp "$(BUILD_DIR)/titanbench" "dist/titanbench-$(TRIPLET)"

clean:
	rm -rf $(BUILD_DIR) dist
