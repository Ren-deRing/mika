U_SRCS := $(shell find . -maxdepth 2 -name "*.c" -o -name "*.S")

U_OBJS := $(patsubst ./%, $(USER_OBJ_DIR)/%, $(U_SRCS))
U_OBJS := $(U_OBJS:.c=.o)
U_OBJS := $(U_OBJS:.S=.o)

USER_PROGS := $(filter-out .,$(notdir $(patsubst %/,%,$(dir $(U_SRCS)))))

USER_CFLAGS  := -g -O1 -ffreestanding -fno-stack-protector \
                -fPIE -fno-omit-frame-pointer \
                -m64 -march=x86-64 -mno-red-zone \
                -I$(BASE_DIR)/user/include \
                -I$(MUSL_OUT_DIR)/include

USER_LDFLAGS := -m elf_x86_64 -nostdlib -static \
                -Ttext 0x400000 \
                -L$(MUSL_OUT_DIR)/lib -lc \
                -z separate-code -z max-page-size=0x1000

.PHONY: all clean libc.a

all: $(USER_PROGS)

$(USER_PROGS): $(U_OBJS)
	echo "[USER LD DYNAMIC] $@"; \
	mkdir -p $(USER_BIN_DIR); \
	$(LD) -m elf_x86_64 -nostdlib -pie \
		-dynamic-linker /lib/ld-musl-x86_64.so.1 \
		-L$(MUSL_OUT_DIR)/lib -lc \
		-z separate-code -z max-page-size=0x1000 \
		$(MUSL_OUT_DIR)/lib/crt1.o \
		$(MUSL_OUT_DIR)/lib/crti.o \
		$(filter $(USER_OBJ_DIR)/$@/%, $(U_OBJS)) \
		$(MUSL_OUT_DIR)/lib/crtn.o \
		-o $(USER_BIN_DIR)/$@; \

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