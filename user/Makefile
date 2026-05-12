U_SRCS := $(shell find . -name "*.c" -o -name "*.S")

U_OBJS := $(patsubst ./%, $(USER_OBJ_DIR)/%, $(U_SRCS))
U_OBJS := $(U_OBJS:.c=.o)
U_OBJS := $(U_OBJS:.S=.o)

USER_PROGS := $(notdir $(patsubst %/,%,$(dir $(U_SRCS))))

USER_CFLAGS  := $(CFLAGS) -fno-pic -fno-pie -I$(BASE_DIR)/user/include
USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -L$(OBJ_DIR)/libc -lc

.PHONY: all clean

all: $(USER_PROGS)

$(USER_PROGS): libc.a $(U_OBJS)
	@mkdir -p $(USER_BIN_DIR)
	@echo "[USER LD] $@"
	@$(LD) $(USER_LDFLAGS) $(filter $(USER_OBJ_DIR)/$@/%, $(U_OBJS)) -o $(USER_BIN_DIR)/$@

libc.a:
	@$(MAKE) -C $(BASE_DIR)/libc

$(USER_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[USER CC] $<"
	@$(CC) $(USER_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(USER_OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "[USER AS] $<"
	@$(CC) $(USER_CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	@rm -rf $(USER_OBJ_DIR)