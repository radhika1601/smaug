# Install prefix of the MPC runtime and EMP toolkit. Uses the repo-local
# .deps prefix when it exists (see docs/install.md), else /usr/local.
DEPS_DIR := $(abspath $(dir $(abspath $(lastword $(MAKEFILE_LIST))))../.deps)
SMAUG_PREFIX ?= $(if $(wildcard $(DEPS_DIR)/lib),$(DEPS_DIR),/usr/local)

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
LLVM_PREFIX ?= /opt/homebrew/opt/llvm@18
else
LLVM_PREFIX ?= /usr/local/opt/llvm@18
endif
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
SDKROOT ?= $(shell xcrun --show-sdk-path)
else
LLVM_PREFIX ?= /usr/lib/llvm-18
OPENSSL_PREFIX ?=
endif

CLANG      ?= $(LLVM_PREFIX)/bin/clang++
OPT        ?= $(LLVM_PREFIX)/bin/opt
PASS_PLUGIN ?= $(dir $(abspath $(lastword $(MAKEFILE_LIST))))../passes/build/libpasses.so
# Lowering run by the link stage: legacy (vec-mpc-link, mpc-remove-ops,
# mpc-link) or new (mpc-narrow-bool, mpc-lower).
MPC_LOWERING ?= legacy
METADATA_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))metadata
REMOVE_TRIPLE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))../passes/remove_target_triple.py
BUILD_DIR  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))build
NOLINK_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))build/no-link
NONGC_DIR  := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))build/no-gc
FLATTEN_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))build/loop-flatten
FLATTEN_NONGC_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))build/loop-flatten-no-gc

CXXFLAGS   := -I$(SMAUG_PREFIX)/include -O0 -Xclang -disable-O0-optnone -S -emit-llvm -std=c++17
LD_ARCH    :=
LD_PATHS   := -L$(SMAUG_PREFIX)/lib -Wl,-rpath,$(SMAUG_PREFIX)/lib
ifeq ($(UNAME_M),x86_64)
LD_ARCH    := -maes -mssse3
endif
ifeq ($(UNAME_S),Darwin)
CXXFLAGS   += -isysroot $(SDKROOT)
LD_PATHS   += -isysroot $(SDKROOT) -L$(OPENSSL_PREFIX)/lib
endif
LD_FLAGS   := $(LD_PATHS) -lssl -lcrypto -lemp-tool $(LD_ARCH) \
              -Wno-deprecated-declarations -lgc-opt
LD_FLAGS_NONGC := $(LD_PATHS) -lssl -lcrypto -lemp-tool $(LD_ARCH) \
              -Wno-deprecated-declarations -lmpc

SRCS  := $(wildcard $(dir $(abspath $(lastword $(MAKEFILE_LIST))))*.cpp)
NAMES := $(notdir $(SRCS:.cpp=))
LLS        := $(addprefix $(BUILD_DIR)/,$(addsuffix .ll,$(NAMES)))
NOLINK_LLS := $(addprefix $(NOLINK_DIR)/,$(addsuffix .ll,$(NAMES)))
BINS       := $(addprefix $(BUILD_DIR)/,$(addsuffix _mpc,$(NAMES)))
NONGC_LLS  := $(addprefix $(NONGC_DIR)/,$(addsuffix .ll,$(NAMES)))
NONGC_BINS := $(addprefix $(NONGC_DIR)/,$(addsuffix _mpc,$(NAMES)))
FLATTEN_LLS  := $(addprefix $(FLATTEN_DIR)/,$(addsuffix .ll,$(NAMES)))
FLATTEN_BINS := $(addprefix $(FLATTEN_DIR)/,$(addsuffix _mpc,$(NAMES)))
FLATTEN_NONGC_LLS  := $(addprefix $(FLATTEN_NONGC_DIR)/,$(addsuffix .ll,$(NAMES)))
FLATTEN_NONGC_BINS := $(addprefix $(FLATTEN_NONGC_DIR)/,$(addsuffix _mpc,$(NAMES)))

.PHONY: all no-link-pipeline no-gc-pipeline loop-flatten-pipeline loop-flatten-no-gc-pipeline clean run run-no-gc run-loop-flatten run-loop-flatten-no-gc force force-no-link force-no-gc force-loop-flatten force-loop-flatten-no-gc

.SECONDARY: $(LLS) $(NOLINK_LLS) $(NONGC_LLS) $(FLATTEN_LLS) $(FLATTEN_NONGC_LLS)

all: $(BINS) $(LLS)

no-link-pipeline: $(NOLINK_LLS)

no-gc-pipeline: $(NONGC_BINS) $(NONGC_LLS)

loop-flatten-pipeline: $(FLATTEN_BINS) $(FLATTEN_LLS)

loop-flatten-no-gc-pipeline: $(FLATTEN_NONGC_BINS) $(FLATTEN_NONGC_LLS)

$(BUILD_DIR) $(NOLINK_DIR) $(NONGC_DIR) $(FLATTEN_DIR) $(FLATTEN_NONGC_DIR):
	mkdir -p $@

# Full smaug-pipeline (gc mode): compile -> strip triple -> opt with link passes
$(BUILD_DIR)/%.ll: $(dir $(abspath $(lastword $(MAKEFILE_LIST))))%.cpp | $(BUILD_DIR)
	$(CLANG) $(CXXFLAGS) $< -o $@
	python3 $(REMOVE_TRIPLE) $@
	$(OPT) --interleave-loops=false --mpc-lowering=$(MPC_LOWERING) \
	    -load-pass-plugin=$(PASS_PLUGIN) \
	    -passes=smaug-pipeline \
	    --metadata-path=$(METADATA_DIR)/$(notdir $@).json \
	    --gc=true \
	    $@ -o $@ -S

# no-link-pipeline: compile -> strip triple -> opt without link passes
$(NOLINK_DIR)/%.ll: $(dir $(abspath $(lastword $(MAKEFILE_LIST))))%.cpp | $(NOLINK_DIR)
	$(CLANG) $(CXXFLAGS) $< -o $@
	python3 $(REMOVE_TRIPLE) $@
	$(OPT) --interleave-loops=false --mpc-lowering=$(MPC_LOWERING) \
	    -load-pass-plugin=$(PASS_PLUGIN) \
	    -passes=no-link-pipeline \
	    --metadata-path=$(METADATA_DIR)/$(notdir $@).json \
	    --gc=true \
	    $@ -o $@ -S

# no-gc-pipeline: smaug-pipeline with gc=false, links against libmpc
$(NONGC_DIR)/%.ll: $(dir $(abspath $(lastword $(MAKEFILE_LIST))))%.cpp | $(NONGC_DIR)
	$(CLANG) $(CXXFLAGS) $< -o $@
	python3 $(REMOVE_TRIPLE) $@
	$(OPT) --interleave-loops=false --mpc-lowering=$(MPC_LOWERING) \
	    -load-pass-plugin=$(PASS_PLUGIN) \
	    -passes=smaug-pipeline \
	    --metadata-path=$(METADATA_DIR)/$(notdir $@).json \
	    --gc=false \
	    $@ -o $@ -S

# Link transformed IR -> MPC binary (gc mode)
$(BINS): $(BUILD_DIR)/%_mpc: $(BUILD_DIR)/%.ll
	$(CLANG) $(LD_FLAGS) $< -std=c++17 -o $@

# Link transformed IR -> MPC binary (non-gc mode)
$(NONGC_BINS): $(NONGC_DIR)/%_mpc: $(NONGC_DIR)/%.ll
	$(CLANG) $(LD_FLAGS_NONGC) $< -std=c++17 -o $@

# loop-flatten-pipeline: two-stage opt to avoid pass-manager interaction hangs.
# Stage 1: vectorize with mpc-loop-flatten/reconstruct (no link passes).
# Stage 2: run the link stage (smaug-link, chosen by MPC_LOWERING) on stage 1 output.
$(FLATTEN_DIR)/%.ll: $(dir $(abspath $(lastword $(MAKEFILE_LIST))))%.cpp | $(FLATTEN_DIR)
	$(CLANG) $(CXXFLAGS) $< -o $@
	python3 $(REMOVE_TRIPLE) $@
	$(OPT) --interleave-loops=false --mpc-lowering=$(MPC_LOWERING) \
	    -load-pass-plugin=$(PASS_PLUGIN) \
	    -passes=no-link-loop-flatten-pipeline \
	    --metadata-path=$(METADATA_DIR)/$(notdir $@).json \
	    --gc=true \
	    $@ -o $@ -S
	$(OPT) --interleave-loops=false --mpc-lowering=$(MPC_LOWERING) \
	    -load-pass-plugin=$(PASS_PLUGIN) \
	    -passes=smaug-link \
	    --metadata-path=$(METADATA_DIR)/$(notdir $@).json \
	    --gc=true \
	    $@ -o $@ -S

# Link transformed IR -> MPC binary (loop-flatten mode)
$(FLATTEN_BINS): $(FLATTEN_DIR)/%_mpc: $(FLATTEN_DIR)/%.ll
	$(CLANG) $(LD_FLAGS) $< -std=c++17 -o $@

# loop-flatten-no-gc-pipeline: two-stage opt with gc=false.
$(FLATTEN_NONGC_DIR)/%.ll: $(dir $(abspath $(lastword $(MAKEFILE_LIST))))%.cpp | $(FLATTEN_NONGC_DIR)
	$(CLANG) $(CXXFLAGS) $< -o $@
	python3 $(REMOVE_TRIPLE) $@
	$(OPT) --interleave-loops=false --mpc-lowering=$(MPC_LOWERING) \
	    -load-pass-plugin=$(PASS_PLUGIN) \
	    -passes=no-link-loop-flatten-pipeline \
	    --metadata-path=$(METADATA_DIR)/$(notdir $@).json \
	    --gc=false \
	    $@ -o $@ -S
	$(OPT) --interleave-loops=false --mpc-lowering=$(MPC_LOWERING) \
	    -load-pass-plugin=$(PASS_PLUGIN) \
	    -passes=smaug-link \
	    --metadata-path=$(METADATA_DIR)/$(notdir $@).json \
	    --gc=false \
	    $@ -o $@ -S

# Link transformed IR -> MPC binary (loop-flatten-no-gc mode)
$(FLATTEN_NONGC_BINS): $(FLATTEN_NONGC_DIR)/%_mpc: $(FLATTEN_NONGC_DIR)/%.ll
	$(CLANG) $(LD_FLAGS_NONGC) $< -std=c++17 -o $@

# Input size passed to each test binary (argv[3]).  Override with: make run N=64
N ?= 16

define run-bins
	@export LD_LIBRARY_PATH=$(SMAUG_PREFIX)/lib:$$LD_LIBRARY_PATH; \
	pass=0; fail=0; port=$(2); \
	for bin in $(1); do \
	    name=$$(basename $$bin _mpc); \
	    port=$$((port+1)); \
	    $$bin 1 $$port $(N) > /tmp/mpc_p1_$$name.log 2>&1 & \
	    PID1=$$!; \
	    $$bin 2 $$port $(N) > /tmp/mpc_p2_$$name.log 2>&1; \
	    RC2=$$?; wait $$PID1; RC1=$$?; \
	    if [ $$RC1 -eq 0 ] && [ $$RC2 -eq 0 ]; then \
	        echo "PASS: $$name"; pass=$$((pass+1)); \
	    else \
	        echo "FAIL: $$name (party1=$$RC1 party2=$$RC2)"; fail=$$((fail+1)); \
	    fi; \
	done; \
	echo ""; echo "Results: $$pass passed, $$fail failed"
endef

run: all
	@mkdir -p data
	$(call run-bins,$(BINS),12300)

run-no-gc: no-gc-pipeline
	@mkdir -p data
	$(call run-bins,$(NONGC_BINS),12400)

run-loop-flatten: loop-flatten-pipeline
	@mkdir -p data
	$(call run-bins,$(FLATTEN_BINS),12500)

run-loop-flatten-no-gc: loop-flatten-no-gc-pipeline
	@mkdir -p data
	$(call run-bins,$(FLATTEN_NONGC_BINS),12600)

force:
	rm -f $(LLS) $(BINS)
	$(MAKE) all

force-no-link:
	rm -f $(NOLINK_LLS)
	$(MAKE) no-link-pipeline

force-no-gc:
	rm -f $(NONGC_LLS) $(NONGC_BINS)
	$(MAKE) no-gc-pipeline

force-loop-flatten:
	rm -f $(FLATTEN_LLS) $(FLATTEN_BINS)
	$(MAKE) loop-flatten-pipeline

force-loop-flatten-no-gc:
	rm -f $(FLATTEN_NONGC_LLS) $(FLATTEN_NONGC_BINS)
	$(MAKE) loop-flatten-no-gc-pipeline

clean:
	rm -rf $(BUILD_DIR)
