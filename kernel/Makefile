K_C_SRCS := $(shell find . -name "*.c")
K_C_OBJS := $(patsubst ./%, $(OBJ_DIR)/kernel/%, $(K_C_SRCS))
K_C_OBJS := $(K_C_OBJS:.c=.o)

K_S_SRCS := $(shell find . -name "*.S")
K_S_OBJS := $(patsubst ./%, $(OBJ_DIR)/kernel/%, $(K_S_SRCS))
K_S_OBJS := $(K_S_OBJS:.S=.o)

K_OBJS := $(K_C_OBJS) $(K_S_OBJS)

BASE_CFLAGS := $(CFLAGS) -mcmodel=kernel -mno-red-zone \
               -I $(BASE_DIR)/include \
               -I $(BASE_DIR) \
               -I $(BASE_DIR)/libc/include \
			   -I $(BASE_DIR)/lib \
			   -mno-sse -mno-sse2 -mno-mmx -mno-avx

KASAN_CFLAGS := -target x86_64-pc-linux-gnu -ffreestanding

NO_KASAN_PATHS := \
    mm/kasan.c \
    mm/kmem.c \
    lib/kprintf.c \
    lib/lock.c \
    misc/kstack.c \
    arch/x86_64/mmu.c \
    arch/x86_64/idt.c \
    arch/x86_64/gdt.c \
    arch/x86_64/early.c \
    arch/x86_64/intc/acpi.c \
    arch/x86_64/io.c \
    generic.c

NO_KASAN_OBJS := $(addprefix $(OBJ_DIR)/kernel/, $(NO_KASAN_PATHS))
NO_KASAN_OBJS := $(NO_KASAN_OBJS:.c=.o)

GEN_DIR := $(BUILD_DIR)/gen
ASM_DEFINES_H := $(GEN_DIR)/asm_defines.h

.PHONY: all clean

all: $(K_OBJS)

$(filter-out $(NO_KASAN_OBJS), $(K_C_OBJS)): $(OBJ_DIR)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@$(CC) $(BASE_CFLAGS) $(KASAN_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(NO_KASAN_OBJS): $(OBJ_DIR)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC] (no-asan) $<"
	@$(CC) $(BASE_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(K_S_OBJS): $(OBJ_DIR)/kernel/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "[AS] $<"
	@$(CC) $(BASE_CFLAGS) -x assembler-with-cpp $(CPPFLAGS) -c $< -o $@

$(ASM_DEFINES_H): $(BASE_DIR)/scripts/gen_asm_defs.c $(BASE_DIR)/include/kernel/proc.h
	@echo " [GEN] $<"
	@mkdir -p $(GEN_DIR)
	$(CC) -o $(BASE_DIR)/scripts/gen_asm_defs $(BASE_DIR)/scripts/gen_asm_defs.c -I$(BASE_DIR)/include
	@$(BASE_DIR)/scripts/gen_asm_defs > $@

$(K_OBJS): $(ASM_DEFINES_H)

clean:
	@rm -rf $(OBJ_DIR)/kernel
	@rm -f $(BASE_DIR)/scripts/gen_asm_defs $(ASM_DEFINES_H)