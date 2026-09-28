# =============================================================================
# Makefile — Unlimited
# =============================================================================
CXX      ?= c++
WARN      = -Wall -Wextra -Wpedantic -Werror
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 $(WARN) -MMD -MP
INCLUDES  = -Isrc -Ipc -Itests

# ── Platform: the live audio backend and the serial/PTY calls of pc/ (spec 12.5) ──
# Linked into every program and test binary built from pc/; the core and check_embedded never see them.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
PC_LIBS   = -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
else ifeq ($(UNAME_S),Linux)
PC_LIBS   = -lasound -lpthread -lutil
else
PC_LIBS   = -lpthread
endif

# ── Directory layout ────────────────────────────────────────────────────────
BUILDDIR  = build
BINDIR    = bin

CORE_SRC    = $(wildcard src/unlimited/*.cpp)
PC_SRC      = $(wildcard pc/*.cpp)
SUPPORT_SRC = $(wildcard tests/support/*.cpp)
TEST_SRC    = $(wildcard tests/*.cpp)
LONG_SRC    = $(wildcard tests/long/*.cpp)
EMBED_SRC   = $(wildcard tests/embedded/*.cpp)

CORE_OBJ    = $(CORE_SRC:%.cpp=$(BUILDDIR)/%.o)
PC_OBJ      = $(PC_SRC:%.cpp=$(BUILDDIR)/%.o)
SUPPORT_OBJ = $(SUPPORT_SRC:%.cpp=$(BUILDDIR)/%.o)
TEST_OBJ    = $(TEST_SRC:%.cpp=$(BUILDDIR)/%.o)
LONG_OBJ    = $(LONG_SRC:%.cpp=$(BUILDDIR)/%.o)
MAIN_OBJ    = $(BUILDDIR)/tests/test_main.o

LIB         = $(BUILDDIR)/libunlimited.a
LIB_DEP     = $(if $(CORE_SRC),$(LIB),)
ENCODE_BIN  = $(BINDIR)/unlimited_encode
DECODE_BIN  = $(BINDIR)/unlimited_decode
TEST_BIN    = $(BINDIR)/unlimited_tests
LONG_BIN    = $(BINDIR)/unlimited_regression

# Core flags for the embedded check: no exceptions, no RTTI, freestanding-friendly.
EMBED_FLAGS = -std=c++11 -O2 $(WARN) -fno-exceptions -fno-rtti -Isrc
# Encoder queue sizes besides the default 64 (the AVR size gate holds for any of them).
QUEUE_VARIANTS = 16 128
# AVR ISR gate (spec 3.9, 8): tx_uno's ISR body, built as Arduino builds a sketch, run on tests/avr/isr_cycles.cpp's
# interpreter for each case of tests/avr/isr_cases.hpp.
AVR_SKETCH_FLAGS = -std=c++11 -Os -flto $(WARN) -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
                   -fno-threadsafe-statics -mmcu=atmega328p -DF_CPU=16000000L -Isrc -Wl,--gc-sections
AVR_ISR_CASES = 0 1 2 3 4 5 6 7
AVR_ENCODER_SRC = src/unlimited/encoder.cpp src/unlimited/protocol.cpp src/unlimited/tables.cpp
AVR_TOOLS = $(if $(AVR_GXX),$(dir $(AVR_GXX)),)
# Soft-float routines: the AVR encoder must never pull them in (B5).
AVR_FLOAT_SYMBOLS = '__(add|sub|mul|div)sf3|__fix(uns)?sfsi|__float(un)?sisf|__(eq|ne|lt|le|gt|ge)sf2'
# Symbols the core must never reference (heap, exceptions, RTTI).
FORBIDDEN_SYMBOLS = 'malloc|calloc|realloc|_Znwm|_Znam|_Znwj|_Znaj|__cxa_throw|__cxa_allocate_exception|typeinfo|__gxx_personality'

# Cross compilers: PATH first, then the toolchains bundled with arduino-cli (macOS and Linux locations).
ARDUINO15   = $(wildcard $(HOME)/Library/Arduino15 $(HOME)/.arduino15)
AVR_GXX     = $(firstword $(shell command -v avr-g++ 2>/dev/null) $(wildcard $(addsuffix /packages/arduino/tools/avr-gcc/*/bin/avr-g++,$(ARDUINO15))))
XTENSA_GXX  = $(firstword $(shell command -v xtensa-esp32-elf-g++ 2>/dev/null) $(wildcard $(addsuffix /packages/esp32/tools/esp-x32/*/bin/xtensa-esp32-elf-g++,$(ARDUINO15))))
ARM_GXX     = $(firstword $(shell command -v arm-none-eabi-g++ 2>/dev/null))

FQBN_UNO   = arduino:avr:uno
FQBN_ESP32 = esp32:esp32:esp32
ARDUINO_BUILD = $(BUILDDIR)/arduino

.PHONY: all lib demo test test_long check_embedded arduino_check demo_run tables docs clean help

all: lib demo

help:
	@echo "make [all|lib|demo|test|test_long|check_embedded|arduino_check|demo_run|tables|docs|clean]"

lib: $(LIB)

demo: $(ENCODE_BIN) $(DECODE_BIN)

$(LIB): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $^

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(ENCODE_BIN): $(BUILDDIR)/demo/unlimited_encode.o $(PC_OBJ) $(LIB_DEP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(PC_LIBS)

$(DECODE_BIN): $(BUILDDIR)/demo/unlimited_decode.o $(PC_OBJ) $(LIB_DEP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(PC_LIBS)

$(TEST_BIN): $(TEST_OBJ) $(SUPPORT_OBJ) $(PC_OBJ) $(LIB_DEP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(PC_LIBS)

$(LONG_BIN): $(MAIN_OBJ) $(LONG_OBJ) $(SUPPORT_OBJ) $(PC_OBJ) $(LIB_DEP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(PC_LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN) $(FILTER)

test_long: $(LONG_BIN)
	./$(LONG_BIN) $(FILTER)

# Core compiled as for an MCU; then prove no heap/exception/RTTI symbol is referenced.
check_embedded:
	@mkdir -p $(BUILDDIR)/embedded
	@for f in $(CORE_SRC); do \
		echo "  embedded $$f"; \
		$(CXX) $(EMBED_FLAGS) -c $$f -o $(BUILDDIR)/embedded/$$(basename $$f .cpp).o || exit 1; \
	done
	@if nm -u $(BUILDDIR)/embedded/*.o | grep -E $(FORBIDDEN_SYMBOLS); then \
		echo "check_embedded: FAILED (forbidden symbol above)"; exit 1; \
	else echo "check_embedded: core is heap/exception/RTTI free"; fi
	@for f in $(EMBED_SRC); do \
		echo "  embedded test $$f"; \
		$(CXX) $(EMBED_FLAGS) $$f $(CORE_SRC) -o $(BUILDDIR)/embedded/$$(basename $$f .cpp) && \
		./$(BUILDDIR)/embedded/$$(basename $$f .cpp) || exit 1; \
	done
	@if [ -n "$(ARM_GXX)" ]; then \
		for f in $(CORE_SRC); do $(ARM_GXX) $(EMBED_FLAGS) -mcpu=cortex-m4 -mthumb -c $$f -o /dev/null || exit 1; done; \
		echo "check_embedded: arm-none-eabi OK"; else echo "check_embedded: arm-none-eabi-g++ not found, skipped"; fi
	@if [ -n "$(XTENSA_GXX)" ]; then \
		for f in $(CORE_SRC); do $(XTENSA_GXX) $(EMBED_FLAGS) -mlongcalls -c $$f -o /dev/null || exit 1; done; \
		echo "check_embedded: xtensa-esp32 OK (the decoder's size static_assert included)"; \
	else echo "check_embedded: xtensa-esp32-elf-g++ not found, skipped"; fi
	@if [ -n "$(AVR_GXX)" ]; then \
		for f in $(CORE_SRC); do $(AVR_GXX) $(EMBED_FLAGS) -mmcu=atmega328p -c $$f -o /dev/null || exit 1; done; \
		for q in $(QUEUE_VARIANTS); do \
			$(AVR_GXX) $(EMBED_FLAGS) -mmcu=atmega328p -DUNLIMITED_ENCODER_QUEUE=$$q -c src/unlimited/encoder.cpp \
				-o /dev/null || exit 1; \
		done; \
		echo "check_embedded: avr (atmega328p) OK, encoder queues 64 $(QUEUE_VARIANTS)"; \
		$(CXX) -std=c++11 -O2 $(WARN) -Isrc tests/avr/isr_cycles.cpp $(AVR_ENCODER_SRC) \
			-o $(BUILDDIR)/embedded/isr_cycles || exit 1; \
		for c in $(AVR_ISR_CASES); do \
			out=$(BUILDDIR)/embedded/avr_isr_$$c; \
			$(AVR_GXX) $(AVR_SKETCH_FLAGS) -DUNLIMITED_ISR_CASE=$$c tests/avr/isr_harness.cpp $(AVR_ENCODER_SRC) \
				-o $$out.elf || exit 1; \
			$(AVR_TOOLS)avr-objdump -d $$out.elf > $$out.dis && \
			$(AVR_TOOLS)avr-objcopy -O binary -j .text -j .data $$out.elf $$out.bin && \
			$(AVR_TOOLS)avr-nm $$out.elf > $$out.sym || exit 1; \
			if $(AVR_TOOLS)avr-nm -u $$out.elf | grep -E $(AVR_FLOAT_SYMBOLS); then \
				echo "check_embedded: FAILED (float routine in the AVR encoder, above)"; exit 1; fi; \
		done; \
		./$(BUILDDIR)/embedded/isr_cycles $(BUILDDIR)/embedded/avr_isr || exit 1; \
	else echo "check_embedded: avr-g++ not found, skipped"; fi

# Compiles the Arduino examples against this library (needs arduino-cli and the cores): tx_uno for the Uno
# (it drives ATmega328P timers), every *_esp32 example for the ESP32. Fails on any warning from the library
# or a sketch; the cores' own warnings are ignored. Logs and flash/RAM use go to build/arduino/.
arduino_check:
	@command -v arduino-cli >/dev/null || { echo "arduino-cli not found"; exit 1; }
	@mkdir -p $(ARDUINO_BUILD)
	@for job in $(FQBN_UNO)@examples/arduino/tx_uno $(foreach d,$(wildcard examples/arduino/*_esp32),$(FQBN_ESP32)@$(d)); do \
		fqbn=$${job%@*}; sketch=$${job#*@}; name=$$(basename $$sketch); log=$(ARDUINO_BUILD)/$$name.log; \
		echo "  arduino $$fqbn $$sketch"; \
		arduino-cli compile --fqbn $$fqbn --warnings all --library $(CURDIR) --build-path $(ARDUINO_BUILD)/$$name \
			$$sketch > $$log 2>&1 || { cat $$log; exit 1; }; \
		if grep -E -A3 "$(CURDIR)/(src|examples)/[^:]+:[0-9]+:[0-9]+: warning:" $$log; then \
			echo "arduino_check: FAILED (warning above, full log in $$log)"; exit 1; \
		fi; \
		grep -E "^(Sketch uses|Global variables)" $$log; \
	done
	@elf=$(ARDUINO_BUILD)/tx_uno/tx_uno.ino.elf; nm=$(AVR_TOOLS)avr-nm; \
	if [ -z "$(AVR_TOOLS)" ]; then echo "arduino_check: avr-nm not found, float check of tx_uno skipped"; \
	elif $$nm $$elf | grep -E $(AVR_FLOAT_SYMBOLS); then \
		echo "arduino_check: FAILED (tx_uno links a float routine, above)"; exit 1; \
	else echo "arduino_check: tx_uno has no float routine"; fi
	@echo "arduino_check: all examples compile warning-free"

# Encode → channel → decode round trips (spec 7): the text must come back exactly. Both sides are told the same speed
# (--bps); the receiver finds the pitch. Each run: name, channel, speed (bytes/s), SNR (dB; am/fm: carrier), TX sample
# rate, frequency offset (Hz; lsb: the shift after the inversion), then the extra encoder and decoder options ('+'
# separates words, '-' = none). Then a configuration the sender must refuse.
DEMO_TEXT = CQ CQ DE UNLIMITED TEST 0123456789
DEMO_DIR  = $(BUILDDIR)/demo_run
DEMO_RUNS = "usb_6 usb 6 10 8000 80 - -" \
            "lsb_12_48k lsb 12 13 48000 -150 - -" \
            "usb_1_narrow usb 1 0 8000 40 --tone+1200+--passband+300:2100 --passband+300:2100" \
            "usb_3_vox usb 3 6 8000 -35 --vox-lead-ms+150 -" \
            "usb_6_auto usb 6 8 22050 25 - --threshold+auto" \
            "am_12 am 12 12 8000 60 --passband+100:3000 --passband+100:3000" \
            "fm_25 fm 25 22 8000 60 --passband+300:3000 --passband+300:3000"
# 25 bytes/s occupies 1100 Hz (950-2050 Hz): it cannot fit a 500 Hz passband.
DEMO_REFUSED = --bps 25 --passband 1250:1750
demo_run: demo
	@mkdir -p $(DEMO_DIR)
	@printf '%s' "$(DEMO_TEXT)" > $(DEMO_DIR)/expect.txt
	@for run in $(DEMO_RUNS); do \
		set -- $$run; name=$$1; ch=$$2; speed=$$3; snr=$$4; rate=$$5; offset=$$6; \
		tx=$$(echo "$$7" | tr '+' ' '); rx=$$(echo "$$8" | tr '+' ' '); \
		if [ "$$tx" = - ]; then tx=; fi; if [ "$$rx" = - ]; then rx=; fi; \
		echo "== $$name: $$ch channel, $$speed bytes/s$${tx:+ $$tx}$${rx:+, receiver $$rx}, SNR $$snr dB, offset $$offset Hz, TX rate $$rate Hz"; \
		./$(ENCODE_BIN) --text "$(DEMO_TEXT)" --bps $$speed --rate $$rate $$tx \
			--channel $$ch --snr $$snr --offset $$offset --out $(DEMO_DIR)/$$name.wav \
			--clean-out $(DEMO_DIR)/$${name}_clean.wav || exit 1; \
		./$(DECODE_BIN) --in $(DEMO_DIR)/$$name.wav --bps $$speed $$rx \
			--expect $(DEMO_DIR)/expect.txt || exit 1; \
	done
	@echo "== refused: $(DEMO_REFUSED) (1100 Hz of signal in a 500 Hz passband) must fail with exit code 2"
	@./$(ENCODE_BIN) --text "$(DEMO_TEXT)" $(DEMO_REFUSED) --out $(DEMO_DIR)/refused.wav 2> $(DEMO_DIR)/refused.txt; \
		status=$$?; cat $(DEMO_DIR)/refused.txt; \
		if [ $$status -ne 2 ] || ! grep -q "does not fit" $(DEMO_DIR)/refused.txt; then \
			echo "demo_run: FAILED (the encoder must refuse it with exit code 2 and say why; exit code $$status)"; \
			exit 1; \
		fi
	@echo "demo_run: all round trips decoded exactly; the configuration that does not fit was refused"

# Builds tools/gen_tables.cpp and checks that src/unlimited/tables.cpp holds exactly the table it prints.
TABLES_SRC = src/unlimited/tables.cpp
GEN_TABLES = $(BUILDDIR)/tools/gen_tables
tables: $(GEN_TABLES)
	@$(GEN_TABLES) > $(GEN_TABLES).txt
	@awk '/^const uint16_t k_quarter_sine\[/,/^};/' $(TABLES_SRC) | diff -u $(GEN_TABLES).txt - && \
		echo "tables: $(TABLES_SRC) is up to date" || \
		{ echo "tables: $(TABLES_SRC) differs from the tools/gen_tables.cpp output (diff above)"; exit 1; }

$(GEN_TABLES): tools/gen_tables.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@

# Documentation generated from the library itself (spec 8): tools/doc_examples.cpp prints the bit-exact protocol
# examples of docs/protocol_examples.md; tools/doc_figures.cpp draws docs/images/*.svg from the real Encoder,
# sim::Channel and Decoder (the BER figure runs the chain on every core). Both stop with an error when the library
# disagrees with an example of spec.md. Everything is built in $(DOCS_BUILD) first; docs/ is replaced only when both
# tools succeeded (stale figures are removed). Deterministic: a rerun writes the same bytes.
DOCS_DIR     = docs
DOCS_BUILD   = $(BUILDDIR)/docs
DOC_FIGURES  = $(BUILDDIR)/tools/doc_figures
DOC_EXAMPLES = $(BUILDDIR)/tools/doc_examples
docs: $(DOC_FIGURES) $(DOC_EXAMPLES)
	@rm -rf $(DOCS_BUILD) && mkdir -p $(DOCS_BUILD)/images
	./$(DOC_EXAMPLES) > $(DOCS_BUILD)/protocol_examples.md
	./$(DOC_FIGURES) $(DOCS_BUILD)/images
	@mkdir -p $(DOCS_DIR)/images
	@rm -f $(DOCS_DIR)/images/*.svg
	@cp $(DOCS_BUILD)/images/*.svg $(DOCS_DIR)/images/
	@cp $(DOCS_BUILD)/protocol_examples.md $(DOCS_DIR)/protocol_examples.md
	@echo "docs: $(DOCS_DIR)/images/*.svg and $(DOCS_DIR)/protocol_examples.md regenerated"

$(DOC_FIGURES): $(BUILDDIR)/tools/doc_figures.o $(SUPPORT_OBJ) $(PC_OBJ) $(LIB_DEP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(PC_LIBS)

$(DOC_EXAMPLES): $(BUILDDIR)/tools/doc_examples.o $(LIB_DEP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -rf $(BUILDDIR) $(BINDIR)

# Dependency files of this build only; arduino-cli leaves its own under $(ARDUINO_BUILD).
-include $(shell find $(BUILDDIR) -path $(ARDUINO_BUILD) -prune -o -name '*.d' -print 2>/dev/null)
