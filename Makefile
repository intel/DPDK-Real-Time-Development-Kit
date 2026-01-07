# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2025 Intel Corporation
#
# Makefile for Launch-Time Testing Tool (lttt)

# Configuration variables
BUILD_DIR ?= builddir
BUILD_TYPE ?= release
ENABLE_MQTT ?= true
ENABLE_DEBUG ?= false
DPDK_GIT_URL ?= https://github.com/DPDK/dpdk.git
DPDK_GIT_TAG ?= v25.11

# Internal variables
SCRIPT_DIR := $(shell pwd)
EXTERNAL_DIR := $(SCRIPT_DIR)/external
DPDK_DIR := $(EXTERNAL_DIR)/dpdk
DPDK_BUILD_DIR := builddir
DPDK_FULL_BUILD_DIR := $(DPDK_DIR)/$(DPDK_BUILD_DIR)
DPDK_INSTALL_DIR := $(EXTERNAL_DIR)/install
DPDK_LIB_DIR := lib/x86_64-linux-gnu
DPDK_PKGCONFIG_DIR := $(DPDK_INSTALL_DIR)/$(DPDK_LIB_DIR)/pkgconfig
PATCHES_DIR := $(SCRIPT_DIR)/patches
EXECUTABLE := $(BUILD_DIR)/apps/launch-time/lttt

# Meson options
MESON_OPTS := -Dbuildtype=$(BUILD_TYPE) \
              -Denable_mqtt=$(ENABLE_MQTT) \
              -Denable_debug=$(ENABLE_DEBUG)

# Always add PKG_CONFIG_PATH to use the locally installed DPDK
# Handle both lib/pkgconfig and lib/x86_64-linux-gnu/pkgconfig paths
export PKG_CONFIG_PATH := $(DPDK_PKGCONFIG_DIR):$(PKG_CONFIG_PATH)

# Color output
RED := \033[0;31m
GREEN := \033[0;32m
YELLOW := \033[0;33m
BLUE := \033[0;34m
NC := \033[0m # No Color

# Default target
.PHONY: all
all: build

# Help target
.PHONY: help
help:
	@echo "$(BLUE)Launch-Time Testing Tool (lttt) Build System$(NC)"
	@echo ""
	@echo "$(GREEN)Usage:$(NC)"
	@echo "  make [target] [VARIABLE=value]"
	@echo ""
	@echo "$(GREEN)Main Targets:$(NC)"
	@echo "  $(YELLOW)all$(NC)          - Build the project (default, includes DPDK)"
	@echo "  $(YELLOW)build$(NC)        - Build the project (requires DPDK to be installed)"
	@echo "  $(YELLOW)setup$(NC)        - Setup meson build without building"
	@echo "  $(YELLOW)rebuild$(NC)      - Clean and rebuild the project"
	@echo "  $(YELLOW)clean$(NC)        - Clean build artifacts"
	@echo "  $(YELLOW)distclean$(NC)    - Clean everything including DPDK"
	@echo "  $(YELLOW)install$(NC)      - Install the built executable"
	@echo "  $(YELLOW)run$(NC)          - Run the executable (requires sudo)"
	@echo "  $(YELLOW)check$(NC)        - Verify executable exists and show info"
	@echo "  $(YELLOW)help$(NC)         - Show this help message"
	@echo ""
	@echo "$(GREEN)DPDK Targets:$(NC)"
	@echo "  $(YELLOW)dpdk$(NC)         - Clone, patch, build, and install DPDK"
	@echo "  $(YELLOW)dpdk-clone$(NC)   - Clone DPDK from source"
	@echo "  $(YELLOW)dpdk-patch$(NC)   - Apply patches to DPDK source"
	@echo "  $(YELLOW)dpdk-build$(NC)   - Build DPDK (assumes source already cloned)"
	@echo "  $(YELLOW)dpdk-install$(NC) - Install DPDK to external/install directory"
	@echo "  $(YELLOW)dpdk-clean$(NC)   - Clean DPDK build"
	@echo "  $(YELLOW)dpdk-distclean$(NC) - Remove DPDK completely"
	@echo ""
	@echo "$(GREEN)Utility Targets:$(NC)"
	@echo "  $(YELLOW)config$(NC)       - Display current build configuration"
	@echo "  $(YELLOW)show-config$(NC)  - Display configuration and meson options"
	@echo ""
	@echo "$(GREEN)Configuration Variables:$(NC)"
	@echo "  $(YELLOW)BUILD_DIR$(NC)        Build directory (default: builddir)"
	@echo "  $(YELLOW)BUILD_TYPE$(NC)       Build type: release, debug, debugoptimized (default: release)"
	@echo "  $(YELLOW)ENABLE_MQTT$(NC)      Enable MQTT support (default: true)"
	@echo "  $(YELLOW)ENABLE_DEBUG$(NC)     Enable debug logging (default: false)"
	@echo "  $(YELLOW)DPDK_GIT_URL$(NC)     DPDK git repository URL"
	@echo "  $(YELLOW)DPDK_GIT_TAG$(NC)     DPDK git tag/branch (default: v25.11)"
	@echo ""
	@echo "$(GREEN)Examples:$(NC)"
	@echo "  $(BLUE)# Build everything (DPDK + project)$(NC)"
	@echo "  make"
	@echo ""
	@echo "  $(BLUE)# Debug build$(NC)"
	@echo "  make BUILD_TYPE=debug ENABLE_DEBUG=true"
	@echo ""
	@echo "  $(BLUE)# Clean rebuild$(NC)"
	@echo "  make rebuild"
	@echo ""
	@echo "  $(BLUE)# Use specific DPDK version$(NC)"
	@echo "  make distclean DPDK_GIT_TAG=v24.11"
	@echo "  make"
	@echo ""

# Configuration display
.PHONY: config
config:
	@echo "$(BLUE)=== Launch Time Build Configuration ===$(NC)"
	@echo "Build directory: $(BUILD_DIR)"
	@echo "Build type: $(BUILD_TYPE)"
	@echo "Enable MQTT: $(ENABLE_MQTT)"
	@echo "Enable debug: $(ENABLE_DEBUG)"
	@echo "DPDK install directory: $(DPDK_INSTALL_DIR)"
	@echo "DPDK pkgconfig directory: $(DPDK_PKGCONFIG_DIR)"
	@echo "DPDK git URL: $(DPDK_GIT_URL)"
	@echo "DPDK git tag: $(DPDK_GIT_TAG)"
	@echo "$(BLUE)========================================$(NC)"

# Setup meson build - ensures DPDK is installed first
.PHONY: setup
setup: config
	@if [ ! -f "$(DPDK_PKGCONFIG_DIR)/libdpdk.pc" ]; then \
	    echo "PKG_CONFIG_PATH: $(PKG_CONFIG_PATH)"; \
		echo "$(YELLOW)DPDK not found, building...$(NC)"; \
		$(MAKE) dpdk; \
	fi
	@echo "$(GREEN)Setting up RTDK Meson build...$(NC)"
	@meson setup $(MESON_OPTS) $(BUILD_DIR) || \
		(echo "$(YELLOW)Build directory exists, reconfiguring...$(NC)" && \
		 meson setup --reconfigure $(MESON_OPTS) $(BUILD_DIR))

# Build the project
.PHONY: build
build: setup
	@echo "$(GREEN)Building Launch Time...$(NC)"
	@ninja -C $(BUILD_DIR)
	@echo ""
	@echo "$(GREEN)=== Build Complete ===$(NC)"

# Rebuild (clean + build)
.PHONY: rebuild
rebuild: clean build

# Clean build artifacts
.PHONY: clean
clean:
	@echo "$(YELLOW)Cleaning build directory...$(NC)"
	@rm -rf $(BUILD_DIR)

# Complete clean (including DPDK)
.PHONY: distclean
distclean: clean dpdk-distclean
	@echo "$(GREEN)All build artifacts cleaned$(NC)"

# Install target
.PHONY: install
install: build
	@echo "$(GREEN)Installing...$(NC)"
	@ninja -C $(BUILD_DIR) install

# Run target (requires sudo)
.PHONY: run
run: build
	@echo "$(GREEN)Running lttt (requires sudo)...$(NC)"
	@sudo $(EXECUTABLE) --help

# DPDK-related targets
.PHONY: dpdk
dpdk: dpdk-clone dpdk-patch dpdk-build dpdk-install
	@echo "$(GREEN)DPDK setup complete$(NC)"

# Clone DPDK from source
.PHONY: dpdk-clone
dpdk-clone:
	@echo "$(BLUE)=== Cloning DPDK from source ===$(NC)"
	@mkdir -p $(EXTERNAL_DIR)
	@if [ ! -d "$(DPDK_DIR)" ]; then \
		echo "$(GREEN)Cloning DPDK from $(DPDK_GIT_URL) (tag: $(DPDK_GIT_TAG))...$(NC)"; \
		git clone --branch $(DPDK_GIT_TAG) --depth 1 $(DPDK_GIT_URL) $(DPDK_DIR); \
	else \
		echo "$(YELLOW)DPDK directory already exists: $(DPDK_DIR)$(NC)"; \
	fi

# Apply DPDK patches
.PHONY: dpdk-patch
dpdk-patch:
	@if [ -d "$(PATCHES_DIR)" ]; then \
		echo "$(GREEN)Applying patches from $(PATCHES_DIR)...$(NC)"; \
		cd $(DPDK_DIR) && \
		if [ ! -f ".tsn_tb_patches_applied" ]; then \
			for patch in $(PATCHES_DIR)/*.patch; do \
				if [ -f "$$patch" ]; then \
					echo "  Applying $$(basename $$patch)..."; \
					git apply "$$patch" 2>/dev/null || \
					(echo "$(YELLOW)  Warning: Patch $$(basename $$patch) failed or already applied$(NC)" && \
					 git apply --reject "$$patch" 2>/dev/null || true); \
				fi; \
			done; \
			touch .tsn_tb_patches_applied; \
			echo "$(GREEN)Patches applied successfully$(NC)"; \
		else \
			echo "$(YELLOW)Patches already applied (marker file found)$(NC)"; \
		fi; \
	else \
		echo "$(YELLOW)No patches directory found, skipping patch application$(NC)"; \
	fi

# Build DPDK
.PHONY: dpdk-build
dpdk-build:
	@if [ ! -d "$(DPDK_FULL_BUILD_DIR)" ]; then \
		echo "$(GREEN)Building DPDK...$(NC)"; \
		cd $(DPDK_DIR) && \
		echo "Configuring DPDK with meson..." && \
		meson setup $(DPDK_BUILD_DIR) -Dexamples='' -Dtests=false --prefix=$(DPDK_INSTALL_DIR) --libdir=$(DPDK_LIB_DIR) && \
		echo "Compiling DPDK..." && \
		ninja -C $(DPDK_BUILD_DIR) && \
		echo "$(GREEN)DPDK build complete$(NC)"; \
	else \
		echo "$(YELLOW)DPDK already built at $(DPDK_FULL_BUILD_DIR)$(NC)"; \
	fi

# Install DPDK to local directory
.PHONY: dpdk-install
dpdk-install:
	@if [ ! -f "$(DPDK_PKGCONFIG_DIR)/libdpdk.pc" ]; then \
		echo "$(GREEN)Installing DPDK to $(DPDK_INSTALL_DIR)...$(NC)"; \
		cd $(DPDK_DIR) && \
		ninja -C $(DPDK_BUILD_DIR) install && \
		echo "$(GREEN)DPDK installation complete$(NC)"; \
	else \
		echo "$(YELLOW)DPDK already installed at $(DPDK_INSTALL_DIR)$(NC)"; \
	fi

# Clean DPDK build
.PHONY: dpdk-clean
dpdk-clean:
	@echo "$(YELLOW)Cleaning DPDK build and installation...$(NC)"
	@rm -rf $(DPDK_FULL_BUILD_DIR)
	@rm -rf $(DPDK_INSTALL_DIR)
	@rm -f $(DPDK_DIR)/.tsn_tb_patches_applied

# Remove DPDK completely
.PHONY: dpdk-distclean
dpdk-distclean:
	@echo "$(YELLOW)Removing DPDK completely...$(NC)"
	@rm -rf $(DPDK_DIR)
	@rm -rf $(DPDK_INSTALL_DIR)

# Test if executable exists
.PHONY: check
check:
	@if [ -f "$(EXECUTABLE)" ]; then \
		echo "$(GREEN)Executable exists: $(EXECUTABLE)$(NC)"; \
		file $(EXECUTABLE); \
	else \
		echo "$(RED)Executable not found: $(EXECUTABLE)$(NC)"; \
		echo "Run 'make build' first"; \
		exit 1; \
	fi

# Show build configuration
.PHONY: show-config
show-config: config
	@echo ""
	@echo "$(BLUE)Meson Options:$(NC)"
	@echo "$(MESON_OPTS)"

# Phony target to prevent conflicts
.PHONY: all help config setup build rebuild clean distclean install run \
        dpdk dpdk-clone dpdk-patch dpdk-build dpdk-install dpdk-clean dpdk-distclean \
        check show-config
