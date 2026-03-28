#
# Makefile for ng_mss_rewrite test infrastructure
#
# This Makefile builds test utilities and provides convenient targets
# for running the test suites.
#

# Build targets
PROG=		ng_builder_generic
SRCS=		ng_builder_generic.c
LDADD=		-lnetgraph
MAN=		# No man page

# Kernel module build
KMOD_DIR=	.
KMOD_MAKEFILE=	Makefile.kmod
KMOD_NAME=	ng_mbuf_inject.ko

# Test scripts
TEST_CASES=	test_cases.sh
TEST_MBUF=	test_mbuf_shape.sh
TEST_WIRE=	test_wire_image.sh
TEST_UPPER=	test_upper_direction.sh
TEST_FUZZ=	test_fuzz.sh

# Targets that require root
SUDO=		sudo

.include <bsd.prog.mk>

# Build kernel module
kmod: ${KMOD_NAME}

${KMOD_NAME}: ng_mbuf_inject.c ${KMOD_MAKEFILE}
	@echo "Building kernel module ${KMOD_NAME}..."
	@$(MAKE) -f ${KMOD_MAKEFILE}

# Build everything
all: ${PROG} kmod

# Clean everything including kernel module artifacts
clean: clean-prog clean-kmod

clean-prog:
	@echo "Cleaning userspace program..."
	@rm -f ${PROG} ${PROG}.full ${PROG}.debug *.pieo *.o .depend.* export_syms

clean-kmod:
	@echo "Cleaning kernel module..."
	@rm -f ${KMOD_NAME} ng_mbuf_inject.o .depend.ng_mbuf_inject.o
	@rm -f export_syms opt_global.h
	@rm -f machine x86 i386

# Install/load kernel module (requires root)
load: kmod
	@echo "Loading ${KMOD_NAME}..."
	@if ${SUDO} kldstat | grep -q ng_mbuf_inject; then \
		${SUDO} kldunload ng_mbuf_inject; \
	fi
	@${SUDO} kldload ./${KMOD_NAME}
	@echo "${KMOD_NAME} loaded successfully"

# Unload kernel module (requires root)
unload:
	@echo "Unloading ${KMOD_NAME}..."
	@${SUDO} kldunload ng_mbuf_inject 2>/dev/null || echo "Module not loaded"

# Test targets (require root)
test: test-all

test-all: ${PROG} kmod
	@echo "========================================="
	@echo " Running all test suites"
	@echo "========================================="
	@echo ""
	@${SUDO} ./${TEST_CASES}
	@echo ""
	@${SUDO} ./${TEST_MBUF}
	@echo ""
	@${SUDO} ./${TEST_WIRE}
	@echo ""
	@${SUDO} ./${TEST_UPPER}

test-cases: ${PROG} kmod
	@echo "Running test_cases.sh..."
	@${SUDO} ./${TEST_CASES}

test-mbuf: ${PROG} kmod
	@echo "Running test_mbuf_shape.sh..."
	@${SUDO} ./${TEST_MBUF}

test-wire: ${PROG} kmod
	@echo "Running test_wire_image.sh..."
	@${SUDO} ./${TEST_WIRE}

test-upper: ${PROG} kmod
	@echo "Running test_upper_direction.sh..."
	@${SUDO} ./${TEST_UPPER}

test-fuzz: ${PROG} kmod
	@echo "Running test_fuzz.sh..."
	@${SUDO} ./${TEST_FUZZ}

# Quick test - just rebuild and run core tests
quick-test: all test-cases test-wire

# Help target
help:
	@echo "ng_mss_rewrite Test Infrastructure Makefile"
	@echo ""
	@echo "Build targets:"
	@echo "  all          - Build ng_builder_generic and ng_mbuf_inject.ko (default)"
	@echo "  kmod         - Build ng_mbuf_inject.ko kernel module only"
	@echo "  clean        - Remove all build artifacts"
	@echo ""
	@echo "Module management (requires root):"
	@echo "  load         - Load ng_mbuf_inject.ko kernel module"
	@echo "  unload       - Unload ng_mbuf_inject.ko kernel module"
	@echo ""
	@echo "Test targets (require root):"
	@echo "  test         - Run all test suites (test-all)"
	@echo "  test-all     - Run all test suites"
	@echo "  test-cases   - Run test_cases.sh (30 core tests)"
	@echo "  test-mbuf    - Run test_mbuf_shape.sh (mbuf boundary tests)"
	@echo "  test-wire    - Run test_wire_image.sh (wire format verification)"
	@echo "  test-upper   - Run test_upper_direction.sh (upper direction tests)"
	@echo "  test-fuzz    - Run test_fuzz.sh (fuzz testing)"
	@echo "  quick-test   - Rebuild and run core tests (test-cases + test-wire)"
	@echo ""
	@echo "Examples:"
	@echo "  make              # Build everything"
	@echo "  make clean all    # Clean rebuild"
	@echo "  make test         # Run all tests"
	@echo "  make quick-test   # Quick test cycle"
	@echo "  make load         # Load kernel module"
	@echo ""

.PHONY: all kmod clean clean-prog clean-kmod load unload \
	test test-all test-cases test-mbuf test-wire test-upper test-fuzz \
	quick-test help
