UNAME_S := $(shell uname -s)

# ── Layout ────────────────────────────────────────────────
SRC_DIR     = src
INC_DIR     = include
VENDOR_DIR  = vendor
TEST_DIR    = tests
FUZZ_DIR    = fuzz
DATA_DIR    = data
BUILD       = build
WBUILD      = $(BUILD)/whisper

# ── Compiler & flags ──────────────────────────────────────
CC      ?= gcc

# Common preprocessor flags shared by every build flavour. Feature-test
# macros must be defined BEFORE any libc header is processed; supplying
# them via -D guarantees that for every TU.
COMMON_CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
                  -I$(INC_DIR) -I$(VENDOR_DIR)

# macOS hides BSD extensions (mkstemps, etc.) under strict _POSIX_C_SOURCE;
# _DARWIN_C_SOURCE re-exposes them. No-op on Linux.
ifeq ($(UNAME_S),Darwin)
  COMMON_CPPFLAGS += -D_DARWIN_C_SOURCE
endif

# Production build (also shared with the in-tree test build).
CFLAGS  ?= -O2 -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
           -Wstrict-prototypes -Wmissing-prototypes \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
           -std=c11 -pthread
CFLAGS  += $(COMMON_CPPFLAGS) \
           -I$(VENDOR_DIR)/whisper.cpp/include \
           -I$(VENDOR_DIR)/whisper.cpp/ggml/include

# Sanitizer/fuzz builds: smaller flag set, but must still carry feature
# macros so glibc exposes the same symbols (timespec, mkstemps, etc.).
SAN_CFLAGS  = -O1 -g -fno-omit-frame-pointer -Wall -Wextra -std=c11 \
              $(COMMON_CPPFLAGS)

# whisper.cpp static libs (built via its own CMake)
WHISPER_A   = $(WBUILD)/src/libwhisper.a
GGML_A      = $(WBUILD)/ggml/src/libggml.a
GGML_BASE   = $(WBUILD)/ggml/src/libggml-base.a
GGML_CPU    = $(WBUILD)/ggml/src/libggml-cpu.a

# Platform link flags
ifeq ($(UNAME_S),Darwin)
  # whisper.cpp auto-enables Metal + BLAS backends on macOS; libggml.a's
  # backend registry references their _reg symbols, so these archives must
  # appear AFTER libggml.a on the link line.
  GGML_METAL      = $(WBUILD)/ggml/src/ggml-metal/libggml-metal.a
  GGML_BLAS       = $(WBUILD)/ggml/src/ggml-blas/libggml-blas.a
  GGML_BACKENDS   = $(GGML_METAL) $(GGML_BLAS)
  CXX_STDLIB      = -lc++
  PLATFORM_LDLIBS = -framework CoreAudio -framework AudioToolbox \
                    -framework CoreFoundation -framework Accelerate \
                    -framework Foundation -framework Metal -framework MetalKit
else
  GGML_BACKENDS   =
  CXX_STDLIB      = -lstdc++
  PLATFORM_LDLIBS = -ldl -lgomp
endif

WLIBS   = $(WHISPER_A) $(GGML_A) $(GGML_CPU) $(GGML_BASE) $(GGML_BACKENDS)
LDLIBS  = $(WLIBS) $(PLATFORM_LDLIBS) -lm -lpthread $(CXX_STDLIB)
NPROC  := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Sources / headers / objects
SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/capture.c $(SRC_DIR)/audio.c \
       $(SRC_DIR)/wav.c  $(SRC_DIR)/vad.c     $(SRC_DIR)/util.c \
       $(SRC_DIR)/tty.c
HDRS = $(wildcard $(INC_DIR)/*.h)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))

# Default model
MODEL      ?= tiny.en
MODEL_DIR   = $(or $(XDG_DATA_HOME),$(HOME)/.local/share)/wat
MODEL_FILE  = $(MODEL_DIR)/ggml-$(MODEL).bin
MODEL_URL   = https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$(MODEL).bin

.PHONY: all clean lint model install help test asan ubsan \
        fuzz fuzz-corpus valgrind valgrind-tests valgrind-full

all: $(BUILD)/wat

$(BUILD) $(BUILD)/tests $(BUILD)/fuzz:
	@mkdir -p $@

$(BUILD)/wat: $(OBJS) $(WHISPER_A) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: $(SRC_DIR)/%.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# miniaudio.h's internal definitions trip -Wmissing-prototypes
$(BUILD)/audio.o: $(SRC_DIR)/audio.c $(INC_DIR)/audio.h $(VENDOR_DIR)/miniaudio.h | $(BUILD)
	$(CC) $(CFLAGS) -Wno-missing-prototypes -Wno-unused-function \
	      -c $(SRC_DIR)/audio.c -o $@

# Build whisper.cpp static lib via CMake (truly out-of-tree)
$(WHISPER_A): $(VENDOR_DIR)/whisper.cpp/CMakeLists.txt
	cmake -B $(WBUILD) -S $(VENDOR_DIR)/whisper.cpp \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DWHISPER_BUILD_EXAMPLES=OFF \
		-DWHISPER_BUILD_TESTS=OFF \
		-DGGML_CUDA=OFF
	cmake --build $(WBUILD) -j$(NPROC)

# ── Tests ──────────────────────────────────────────────────
TEST_SRCS = $(TEST_DIR)/run_tests.c $(SRC_DIR)/wav.c $(SRC_DIR)/vad.c \
            $(SRC_DIR)/util.c

test: $(BUILD)/tests/run_tests
	$(BUILD)/tests/run_tests

$(BUILD)/tests/run_tests: $(TEST_SRCS) $(HDRS) | $(BUILD)/tests
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS) -lm

# ── Sanitizer builds ───────────────────────────────────────
asan: $(BUILD)/tests/run_tests_asan
	$(BUILD)/tests/run_tests_asan

$(BUILD)/tests/run_tests_asan: $(TEST_SRCS) $(HDRS) | $(BUILD)/tests
	$(CC) $(SAN_CFLAGS) -fsanitize=address,undefined \
	      -o $@ $(TEST_SRCS) -lm

ubsan: $(BUILD)/tests/run_tests_ubsan
	$(BUILD)/tests/run_tests_ubsan

$(BUILD)/tests/run_tests_ubsan: $(TEST_SRCS) $(HDRS) | $(BUILD)/tests
	$(CC) $(SAN_CFLAGS) -fsanitize=undefined \
	      -o $@ $(TEST_SRCS) -lm

# ── Fuzzing (AFL) ──────────────────────────────────────────
FUZZ_SRCS = $(FUZZ_DIR)/fuzz_wav.c $(SRC_DIR)/wav.c $(SRC_DIR)/util.c

fuzz: $(BUILD)/fuzz/fuzz_wav $(FUZZ_DIR)/seeds/.populated
	@echo "run: afl-fuzz -i $(FUZZ_DIR)/seeds -o $(FUZZ_DIR)/findings \\"
	@echo "         -x $(FUZZ_DIR)/wav.dict -- $(BUILD)/fuzz/fuzz_wav @@"

$(BUILD)/fuzz/fuzz_wav: $(FUZZ_SRCS) $(HDRS) | $(BUILD)/fuzz
	afl-gcc $(SAN_CFLAGS) -o $@ $(FUZZ_SRCS) -lm

$(BUILD)/fuzz/make_corpus: $(FUZZ_DIR)/make_corpus.c $(HDRS) | $(BUILD)/fuzz
	$(CC) -O2 -Wall -I$(INC_DIR) -o $@ $(FUZZ_DIR)/make_corpus.c

# Regenerate the seed corpus from the structured generator. Touched
# .populated marker means "seeds/ holds the current generator's output".
fuzz-corpus: $(FUZZ_DIR)/seeds/.populated

$(FUZZ_DIR)/seeds/.populated: $(BUILD)/fuzz/make_corpus
	@rm -rf $(FUZZ_DIR)/seeds && mkdir -p $(FUZZ_DIR)/seeds
	$(BUILD)/fuzz/make_corpus $(FUZZ_DIR)/seeds
	@touch $@

# ── Valgrind ───────────────────────────────────────────────
# valgrind-tests: exercise wav.c + vad.c + util.c (fast, deterministic)
# valgrind-full:  whole pipeline incl. whisper (slow, ~5+ min)
valgrind: valgrind-tests

valgrind-tests: $(BUILD)/tests/run_tests
	valgrind --leak-check=full --show-leak-kinds=all \
		--error-exitcode=99 --errors-for-leak-kinds=definite,indirect \
		$(BUILD)/tests/run_tests

valgrind-full: $(BUILD)/wat
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=99 \
		$(BUILD)/wat $(VENDOR_DIR)/whisper.cpp/samples/jfk.wav

# ── Static analysis ────────────────────────────────────────
lint:
	cppcheck --enable=all --inline-suppr \
	         --suppress=missingIncludeSystem \
	         --suppress=missingInclude \
	         --suppress=unusedFunction \
	         --suppress=unmatchedSuppression \
	         --suppress=normalCheckLevelMaxBranches \
	         -I$(INC_DIR) $(SRC_DIR)/

model:
	@mkdir -p $(MODEL_DIR)
	curl -L -o $(MODEL_FILE) $(MODEL_URL)

clean:
	rm -rf $(BUILD)
	rm -rf $(FUZZ_DIR)/findings
	rm -f  $(FUZZ_DIR)/seeds/.populated

install: $(BUILD)/wat
	install -m 755 $(BUILD)/wat /usr/local/bin/

help:
	@echo "targets:"
	@echo "  all          build wat (default) → build/wat"
	@echo "  test         run unit tests"
	@echo "  asan         run tests under AddressSanitizer + UBSan"
	@echo "  ubsan        run tests under UBSan"
	@echo "  fuzz         build AFL harness; prints command to start fuzzing"
	@echo "  fuzz-corpus  (re)generate diverse WAV seed corpus for AFL"
	@echo "  valgrind     run tests under valgrind"
	@echo "  lint         cppcheck static analysis"
	@echo "  model        download whisper model (MODEL=tiny.en|base.en|...)"
	@echo "  clean        remove build/ and fuzz findings"
	@echo "  install      copy build/wat to /usr/local/bin"
