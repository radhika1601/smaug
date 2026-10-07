# Output correctness tests: each program is run under MPC and its result is
# compared with a native build of the same source. Two suites: the benchmarks,
# and the operation tests in ops/. See check_outputs.py.
#
#   make check                               benchmarks and ops, all pipelines
#   make check SUITES=ops PIPELINES=gc,no-gc   one suite, some pipelines
#   make check TESTS=count10,ops/xor N=64      some tests, other size
#   make check LOWERING=legacy,new             compare the two lowerings
#
# Tool locations follow benchmarks/Makefile: LLVM_PREFIX, SMAUG_PREFIX and
# OPENSSL_PREFIX can be set in the environment.

PYTHON     ?= python3
PIPELINES  ?= gc,no-gc,loop-flatten,loop-flatten-no-gc
SUITES     ?= benchmarks,ops
LOWERING   ?= legacy
TESTS      ?=
N          ?= 16
SEEDS      ?= 1,2

ARGS := --suites $(SUITES) --pipelines $(PIPELINES) --lowering $(LOWERING) \
        -n $(N) --seeds $(SEEDS)
ifneq ($(TESTS),)
ARGS += --tests $(TESTS)
endif

.PHONY: check clean

check:
	$(PYTHON) check_outputs.py $(ARGS)

clean:
	rm -rf build
