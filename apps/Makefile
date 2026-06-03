APP_MAKEFILES := $(shell find . -mindepth 2 -maxdepth 3 -name "Makefile")
APP_DIRS      := $(patsubst %/Makefile, %, $(APP_MAKEFILES))

export ARCH         ?= x86_64
export BASE_DIR     ?= $(shell pwd)/..
export MUSL_OUT_DIR ?= $(BASE_DIR)/build/musl
export USER_BIN_DIR ?= $(BASE_DIR)/build/rootfs/bin

export APPS_CC      := clang -target $(ARCH)-unknown-none-elf
export APPS_CFLAGS  := -g -O2 -ffreestanding -fno-stack-protector \
                       -fno-pic -fno-pie -fno-omit-frame-pointer \
                       -m64 -march=x86-64 -mno-red-zone \
                       -I$(BASE_DIR)/user/include \
                       -I$(MUSL_OUT_DIR)/include

export APPS_LDFLAGS := -fuse-ld=lld -nostdlib -static \
                       -Wl,-Ttext,0x400000 \
                       -Wl,-z,separate-code -Wl,-z,max-page-size=0x1000

export APPS_LIBS    := $(MUSL_OUT_DIR)/lib/crt1.o \
                       $(MUSL_OUT_DIR)/lib/crti.o \
                       -L$(MUSL_OUT_DIR)/lib -lc -lm \
                       $(MUSL_OUT_DIR)/lib/crtn.o

-include $(wildcard */apps.mk)
-include $(wildcard */*/apps.mk)
-include $(wildcard */*/*/apps.mk)

.PHONY: all clean $(APP_DIRS)

all: $(APP_DIRS)

$(APP_DIRS):
	$(eval REL_DIR := $(patsubst ./%,%,$@))
	@if echo "$(EXCLUDE_DIRS)" | grep -q "$(REL_DIR)"; then \
		echo "[APPS] Skipping excluded directory: $(REL_DIR)"; \
	else \
		echo "[APPS] Preparing build for $(REL_DIR)..."; \
		$(if $($(REL_DIR)_PRE_BUILD), \
			echo "[APPS PRE_BUILD] Running pre-build hook for $(REL_DIR)"; \
			$($(REL_DIR)_PRE_BUILD); \
		) \
		echo "[APPS MAKE] Compiling in $@"; \
		$(MAKE) -C $@ \
			CC="$(APPS_CC)" \
			CFLAGS="$(APPS_CFLAGS) $($(REL_DIR)_CFLAGS) -I$@" \
			LDFLAGS="$(APPS_LDFLAGS) $($(REL_DIR)_LDFLAGS)" \
			LIBS="$(APPS_LIBS) $($(REL_DIR)_LIBS)" \
			NOSDL=1 \
			OUTPUT="$(notdir $@)"; \
		mkdir -p $(USER_BIN_DIR); \
		O_FILE="$($(REL_DIR)_OUTPUT_FILE)"; \
		if [ -z "$$O_FILE" ]; then \
			O_FILE="$(notdir $@)"; \
		fi; \
		INSTALL_NAME="$($(REL_DIR)_INSTALL_NAME)"; \
		if [ -z "$$INSTALL_NAME" ]; then \
			INSTALL_NAME=$$(basename $$O_FILE); \
		fi; \
		if [ -f "$@/$$O_FILE" ]; then \
			echo "[APPS INSTALL] Installing $@/$$O_FILE -> $(USER_BIN_DIR)/$$INSTALL_NAME"; \
			cp -v "$@/$$O_FILE" "$(USER_BIN_DIR)/$$INSTALL_NAME"; \
		fi \
	fi

clean:
	@for dir in $(APP_DIRS); do \
		REL_DIR=$$(echo $$dir | sed 's|^\./||'); \
		if echo "$(EXCLUDE_DIRS)" | grep -q "$$REL_DIR"; then \
			continue; \
		fi; \
		echo "[APPS CLEAN] Cleaning in $$dir"; \
		$(MAKE) -C $$dir clean OUTPUT=$$(basename $$dir) || true; \
		O_FILE="$$($${REL_DIR}_OUTPUT_FILE)"; \
		if [ -z "$$O_FILE" ]; then \
			O_FILE=$$(basename $$dir); \
		fi; \
		INSTALL_NAME="$$($${REL_DIR}_INSTALL_NAME)"; \
		if [ -z "$$INSTALL_NAME" ]; then \
			INSTALL_NAME=$$(basename $$O_FILE); \
		fi; \
		rm -f $(USER_BIN_DIR)/$$INSTALL_NAME; \
	done