TARGET := libgbm.so
OBJS   := gbm.o

override CFLAGS += -fPIC -DPIC -I.

.PHONY: all clean install

all: $(TARGET) install

$(TARGET): $(OBJS)
	@echo "[LD] Linking shared library $@"
	$(CC) -target $(ARCH)-pc-linux-musl -fuse-ld=lld -nostdlib -shared -fPIC -Wl,-z,separate-code -Wl,-z,max-page-size=0x1000 -Wl,--hash-style=both -o $@ $(OBJS)

%.o: %.c
	@echo "[CC] Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

install: $(TARGET)
	@echo "[INSTALL] Copying gbm.h to $(MUSL_OUT_DIR)/include/"
	@mkdir -p $(MUSL_OUT_DIR)/include
	@cp -f gbm.h $(MUSL_OUT_DIR)/include/gbm.h
	@echo "[INSTALL] Copying $(TARGET) to $(MUSL_OUT_DIR)/lib/"
	@mkdir -p $(MUSL_OUT_DIR)/lib
	@cp -f $(TARGET) $(MUSL_OUT_DIR)/lib/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
