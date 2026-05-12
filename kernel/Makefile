K_SRCS := $(shell find . -name "*.c" -o -name "*.S")

K_OBJS := $(patsubst ./%, $(OBJ_DIR)/kernel/%, $(K_SRCS))
K_OBJS := $(K_OBJS:.c=.o)
K_OBJS := $(K_OBJS:.S=.o)

K_CFLAGS := $(CFLAGS) -mcmodel=kernel -mno-red-zone \
             -I $(BASE_DIR)/include \
             -I $(BASE_DIR) \
             -I $(BASE_DIR)/libc/include \
			 -I $(BASE_DIR)/lib \
			 -mno-sse -mno-sse2 -mno-mmx -mno-avx

GEN_DIR := $(BUILD_DIR)/gen
ASM_DEFINES_H := $(GEN_DIR)/asm_defines.h

.PHONY: all clean

all: $(K_OBJS)

$(OBJ_DIR)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	@$(CC) $(K_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/kernel/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "[AS] $<"
	@$(CC) $(K_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(ASM_DEFINES_H): $(BASE_DIR)/scripts/gen_asm_defs.c $(BASE_DIR)/include/kernel/proc.h
	@echo " [GEN] $<"
	@mkdir -p $(GEN_DIR)
	$(CC) -o $(BASE_DIR)/scripts/gen_asm_defs $(BASE_DIR)/scripts/gen_asm_defs.c -I$(BASE_DIR)/include
	@$(BASE_DIR)/scripts/gen_asm_defs > $@

$(K_OBJS): $(ASM_DEFINES_H)

clean:
	@rm -rf $(OBJ_DIR)/kernel
	@rm -f $(BASE_DIR)/scripts/gen_asm_defs $(ASM_DEFINES_H)