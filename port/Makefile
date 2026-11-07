# Call of Duty: Strike Team -> Horizon (AArch32) loader port.
# Built inside the vita2hos image, the only local toolchain with 32-bit libnx + deko3D.
# Output is an exefs NSP, launched through Atmosphere's hbl_config override
# (see docs/PORTING.md); hbmenu's .nro path cannot start a 32-bit process.

TARGET   := cod_st
BUILD    := build
LIBNX    := $(DEVKITPRO)/libnx32
CC       := arm-none-eabi-gcc

SOURCES  := src src/rt src/vfs src/bionic src/gl src/loader src/jni
CFILES   := $(foreach d,$(SOURCES),$(wildcard $(d)/*.c))
OFILES   := $(BUILD)/src/rt/exc32.o $(BUILD)/src/bionic/fastmem.o \
            $(BUILD)/src/bionic/runtime.o $(BUILD)/src/loader/so_thunks.o $(BUILD)/src/jni/jni_thunks.o \
            $(CFILES:%.c=$(BUILD)/%.o)

# The libnx32 fork comes first; deko3D and miniz still come from the image.
FORK     := third_party/libnx32/prefix
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mfloat-abi=softfp -mfpu=neon-fp-armv8 \
            -mtp=soft -fPIE -ftls-model=local-exec -marm
CFLAGS   := $(ARCH) -g -O2 -ffunction-sections -fdata-sections -Wall -Wextra \
            -D__SWITCH__ -D__ARM32__ -D_GNU_SOURCE -Iinclude -I$(FORK)/include -I$(LIBNX)/include
LDFLAGS  := -specs=config/dcr32.specs -T config/dcr32.ld $(ARCH) -g -Wl,-Map,$(BUILD)/$(TARGET).map \
            -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=free # src/bionic/alloc_track.c
LIBS     := -L$(FORK)/lib -L$(LIBNX)/lib -lminiz -ldeko3d -lnx -lm

.PHONY: all clean FORCE
all: $(BUILD)/$(TARGET).checked $(BUILD)/$(TARGET).nro.checked

# 32-bit NRO, for a 32-bit forwarder or loader only: hbmenu's 64-bit hbloader cannot
# run it.  See "32-bit NRO" in docs/PORTING.md for what the forwarder must provide.
$(BUILD)/$(TARGET).nacp: Makefile
	@mkdir -p $(BUILD)
	nacptool --create "COD Strike Team Port" "port" "0.1.0" $@

$(BUILD)/$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp
	elf2nro $< $@ --nacp=$(BUILD)/$(TARGET).nacp --icon=$(LIBNX)/default_icon.jpg

$(BUILD)/$(TARGET).nro.checked: $(BUILD)/$(TARGET).nro tools/check_nro.py
	python3 tools/check_nro.py $< $(BUILD)/$(TARGET).elf
	@touch $@

# Runs before relocation and before libc exists, so nothing may become a memset call.
$(BUILD)/src/rt/crt0_reloc.o: CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -g -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OFILES)
	$(CC) $(LDFLAGS) $(OFILES) $(LIBS) -o $@

$(BUILD)/$(TARGET).nso: $(BUILD)/$(TARGET).elf
	elf2nso $< $@

$(BUILD)/$(TARGET).json: tools/gen_npdm.py FORCE
	@mkdir -p $(BUILD)
	python3 tools/gen_npdm.py $@

$(BUILD)/$(TARGET).npdm: $(BUILD)/$(TARGET).json
	npdmtool $< $@

$(BUILD)/$(TARGET).nsp: $(BUILD)/$(TARGET).nso $(BUILD)/$(TARGET).npdm
	@rm -rf $(BUILD)/exefs && mkdir -p $(BUILD)/exefs
	cp $(BUILD)/$(TARGET).nso $(BUILD)/exefs/main
	cp $(BUILD)/$(TARGET).npdm $(BUILD)/exefs/main.npdm
	build_pfs0 $(BUILD)/exefs $@

$(BUILD)/$(TARGET).checked: $(BUILD)/$(TARGET).nsp tools/check_elf.py
	python3 tools/check_elf.py $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).npdm $<
	@touch $@

clean:
	rm -rf $(BUILD)

-include $(OFILES:.o=.d)
