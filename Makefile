BUILD_DIR ?= build
BUILD_TYPE ?= Release
PREFIX ?= $(HOME)/.local
GENERATOR ?= Ninja

CMAKE ?= cmake
CLANG_TIDY ?= clang-tidy
CLAZY ?= clazy-standalone
QMLLINT ?= qmllint
PYTHON ?= python3

LINT_SOURCES := \
	src/main.cpp src/capture.cpp src/editor.cpp src/surface-capture.cpp \
	src/cli-path.cpp src/icons.cpp src/pin.cpp src/pin-file.cpp \
	src/pin-layout.cpp src/eyedropper.cpp src/instance-lock.cpp \
	tests/editor-smoke.cpp tests/transform-smoke.cpp \
	tests/clipboard-smoke.cpp tests/pin-layout-smoke.cpp \
	tests/pin-lifecycle-smoke.cpp tests/instance-lock-smoke.cpp
LINT_CHECKS ?= -*,clang-analyzer-*,bugprone-*,performance-*,misc-*

.PHONY: all configure build clean install check contracts smoke lint qt-lint

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -G $(GENERATOR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX)

build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel

smoke: build
	QT_QPA_PLATFORM=offscreen ./$(BUILD_DIR)/nirisnap-smoke \
		$(BUILD_DIR)/nirisnap-smoke-output

lint: build
	@set -eu; \
	if command -v "$(CLANG_TIDY)" >/dev/null 2>&1; then \
		commands_dir=$$(mktemp -d); \
		trap 'rm -rf "$$commands_dir"' EXIT; \
		cp "$(BUILD_DIR)/compile_commands.json" \
			"$$commands_dir/compile_commands.json"; \
		sed -i 's/ -mno-direct-extern-access//g' \
			"$$commands_dir/compile_commands.json"; \
		for source in $(LINT_SOURCES); do \
			"$(CLANG_TIDY)" -p "$$commands_dir" \
				-checks="$(LINT_CHECKS)" -header-filter='.*' "$$source"; \
		done; \
	else \
		echo "make check: clang-tidy unavailable; skipping"; \
	fi

qt-lint: build
	@set -eu; \
	if command -v "$(CLAZY)" >/dev/null 2>&1; then \
		for source in $(LINT_SOURCES); do \
			"$(CLAZY)" -p "$(BUILD_DIR)" "$$source"; \
		done; \
	else \
		echo "make check: clazy unavailable; skipping Qt-specific clazy pass"; \
	fi; \
	if command -v "$(QMLLINT)" >/dev/null 2>&1 && \
			test -d /etc/xdg/quickshell/noctalia-shell; then \
		"$(QMLLINT)" -I /etc/xdg/quickshell/noctalia-shell \
			plugin/nirisnap/*.qml; \
	else \
		echo "make check: Noctalia or qmllint unavailable; skipping QML lint"; \
	fi

contracts:
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

check: build
	$(MAKE) contracts
	$(MAKE) smoke
	$(MAKE) lint
	$(MAKE) qt-lint

clean:
	@if test -d "$(BUILD_DIR)"; then \
		$(CMAKE) --build "$(BUILD_DIR)" --target clean; \
	fi

install: build
	$(CMAKE) --install $(BUILD_DIR)
