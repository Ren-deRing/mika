BASE_DIR ?= $(shell pwd)/../..

MODULE_CFLAGS := -target x86_64-unknown-none-elf \
                 -g -O2 -ffreestanding -fno-stack-protector \
                 -fno-PIC -fno-PIE -mcmodel=large -m64 -march=x86-64 \
                 -mno-red-zone -ffunction-sections -fdata-sections \
                 -Wall -Wextra -I$(BASE_DIR)/include \
                 -include $(BASE_DIR)/include/kernel/symbol.h

all: hello/hello.ko

hello/hello.ko: hello/hello.c
	@mkdir -p hello
	clang $(MODULE_CFLAGS) -c $< -o $@

clean:
	rm -f hello/hello.ko

.PHONY: all clean
