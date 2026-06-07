OUTPUT := doppio
ARCH   ?= x86_64

export BASE_DIR   := $(shell pwd)
export BUILD_DIR  := $(BASE_DIR)/build
export OBJ_DIR    := $(BUILD_DIR)/obj
export BIN_DIR    := $(BASE_DIR)/bin

export USER_OBJ_DIR := $(OBJ_DIR)/user
export USER_BIN_DIR := $(BUILD_DIR)/rootfs/bin

ISO_IMAGE := $(BIN_DIR)/doppio.iso
ISO_ROOT  := $(BUILD_DIR)/iso_root

INITRD_ROOT := $(BASE_DIR)/rootfs
INITRD_TEMP := $(BUILD_DIR)/initrd_temp
INITRD_IMG  := $(ISO_ROOT)/boot/initrd.img

LIMINE_DIR := $(BASE_DIR)/limine
MUSL_DIR := $(BASE_DIR)/musl

export MUSL_OUT_DIR := $(MUSL_DIR)/build_out

SUBDIRS := lib libc util kernel user apps modules boot

ifeq ($(ARCH), x86_64)
    ARCH_CFLAGS  := -m64 -march=x86-64 -mno-red-zone
    ARCH_LDFLAGS := -m elf_x86_64
    LIMINE_EFI   := $(LIMINE_DIR)/BOOTX64.EFI
else ifeq ($(ARCH), aarch64)
    ARCH_CFLAGS  := -march=armv8-a -mcpu=cortex-a72
    ARCH_LDFLAGS := -m aarch64elf
    LIMINE_EFI   := $(LIMINE_DIR)/BOOTAA64.EFI
endif

export CC      := clang -target $(ARCH)-unknown-none-elf
export LD      := ld.lld
export AS      := nasm 

export CFLAGS  := -g -O2 -ffreestanding -fno-stack-protector \
				  -fno-PIC -fno-PIE \
                  $(ARCH_CFLAGS) \
                  -ffunction-sections -fdata-sections -Wall -Wextra

export CPPFLAGS := -I$(BASE_DIR) \
                   -I$(BASE_DIR)/libc/include \
				   -I$(BASE_DIR)/include \
                   -D__$(ARCH)__

export LDFLAGS  := $(ARCH_LDFLAGS) -nostdlib -static --gc-sections \
				   -no-pie -z max-page-size=0x1000 \
                   -T $(BASE_DIR)/kernel/arch/$(ARCH)/linker.ld

.PHONY: all clean $(SUBDIRS) run setup iso

all: $(BIN_DIR)/$(OUTPUT)

$(SUBDIRS):
	@mkdir -p $(OBJ_DIR)/$@
	@$(MAKE) -C $@ ARCH=$(ARCH)

$(BIN_DIR)/$(OUTPUT): $(SUBDIRS)
	@mkdir -p $(BIN_DIR)
	@echo "[LD] Linking $@"
	$(eval ALL_OBJS := $(shell find $(OBJ_DIR) -name "*.o"))
	$(eval KERNEL_OBJS := $(filter-out $(USER_OBJ_DIR)/%, $(ALL_OBJS)))
	$(LD) $(LDFLAGS) $(shell echo $(KERNEL_OBJS) | tr ' ' '\n' | sort) -o $@

download:
	@if [ ! -d "$(LIMINE_DIR)" ]; then \
		echo "Downloading Limine binaries..."; \
		git clone https://codeberg.org/Limine/Limine.git limine --branch=v10.x-binary --depth=1; \
	fi
	@if [ ! -d "$(MUSL_DIR)" ] || [ ! -f "$(MUSL_DIR)/configure" ]; then \
		echo "Downloading musl source..."; \
		rm -rf $(MUSL_DIR); \
		mkdir -p $(MUSL_DIR); \
		wget https://musl.libc.org/releases/musl-1.2.6.tar.gz; \
		tar -xf musl-1.2.6.tar.gz -C $(MUSL_DIR) --strip-components=1; \
		rm musl-1.2.6.tar.gz; \
	fi

musl_build: download
	@if [ ! -f "$(MUSL_OUT_DIR)/lib/libc.a" ]; then \
		echo "[MUSL] Configuring and building musl libc..."; \
		cd $(MUSL_DIR) && \
		CC="clang -target $(ARCH)-pc-linux-musl" \
		CFLAGS="-g -O2 -ffreestanding -fno-stack-protector -m64 -march=x86-64 -mno-red-zone" \
		LDFLAGS="-fuse-ld=lld" ./configure --prefix=$(MUSL_OUT_DIR) --enable-shared --enable-static && \
		$(MAKE) -j$$(nproc) LDFLAGS="-fuse-ld=lld" && \
		$(MAKE) install LDFLAGS="-fuse-ld=lld"; \
	fi

setup: musl_build

iso: all
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot/limine
	@mkdir -p $(ISO_ROOT)/EFI/BOOT

	@rm -rf $(INITRD_TEMP)
	@mkdir -p $(INITRD_TEMP)/bin

	@if [ -d "$(INITRD_ROOT)" ]; then \
		cp -r $(INITRD_ROOT)/* $(INITRD_TEMP)/; \
	fi

	@if [ -d "$(USER_BIN_DIR)" ]; then \
		cp -rv $(USER_BIN_DIR)/* $(INITRD_TEMP)/bin/ 2>/dev/null || true; \
	fi
	@mkdir -p $(INITRD_TEMP)/lib
	@cp -v $(MUSL_OUT_DIR)/lib/libc.so $(INITRD_TEMP)/lib/libc.so
	@cp -v $(MUSL_OUT_DIR)/lib/libc.so $(INITRD_TEMP)/lib/ld-musl-x86_64.so.1
	@cp -L -v $(MUSL_OUT_DIR)/lib/*.so* $(INITRD_TEMP)/lib/

	@cd $(INITRD_TEMP) && find . -mindepth 1 | cpio -o -H newc > $(INITRD_IMG)

	@cp -v $(BIN_DIR)/$(OUTPUT) $(ISO_ROOT)/boot/
	@cp -v $(BASE_DIR)/limine.conf $(ISO_ROOT)/boot/limine/

	@cp -v $(LIMINE_DIR)/limine-bios.sys \
	       $(LIMINE_DIR)/limine-bios-cd.bin \
	       $(LIMINE_DIR)/limine-uefi-cd.bin \
	       $(ISO_ROOT)/boot/limine/
	@cp -v $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	@cp -v $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/

	@xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
	        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
	        -efi-boot-part --efi-boot-image --protective-msdos-label \
	        $(ISO_ROOT) -o $(ISO_IMAGE)

	# @$(LIMINE_DIR)/limine bios-install $(ISO_IMAGE)

run: iso
	qemu-system-x86_64 \
		-machine q35 \
		-cdrom $(ISO_IMAGE) \
		-m 8G \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-bios /usr/share/ovmf/OVMF.fd \
		-serial stdio -d int,cpu_reset -smp 1 -accel kvm -cpu host \
		-vga std

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)