TARGET  := Save_WebDAV
ifeq ($(strip $(DEVKITPRO)),)
$(error 請先設定 DEVKITPRO)
endif
PREFIX  := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
PORTLIB := $(DEVKITPRO)/portlibs/switch
PKGCFG  := $(PORTLIB)/bin/aarch64-none-elf-pkg-config
PKGENV  := PKG_CONFIG_PATH=$(PORTLIB)/lib/pkgconfig
PKGS    := libcurl libarchive sdl2 SDL2_ttf
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS:= -O2 -ffunction-sections $(ARCH) -D__SWITCH__ -std=gnu++17 \
           -I$(DEVKITPRO)/libnx/include -I$(PORTLIB)/include \
           $(shell $(PKGENV) $(PKGCFG) --cflags $(PKGS) 2>/dev/null)
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) \
           -L$(DEVKITPRO)/libnx/lib -L$(PORTLIB)/lib
LIBS    := $(shell $(PKGENV) $(PKGCFG) --libs --static $(PKGS) 2>/dev/null) -lnx
ifeq ($(strip $(LIBS)),-lnx)
LIBS    := -larchive -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lzstd -llzma -lbz2 -lz \
           -lSDL2_ttf -lfreetype -lSDL2 -lEGL -lglapi -ldrm_nouveau -lnx
endif
TOOLS   := $(DEVKITPRO)/tools/bin

all: $(TARGET).nro

$(TARGET).elf: source/main.cpp
	$(PREFIX)g++ $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LIBS)

$(TARGET).nacp:
	$(TOOLS)/nacptool --create "Save WebDAV" "ted5789" "1.1.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp icon.jpg
	$(TOOLS)/elf2nro $(TARGET).elf $@ --nacp=$(TARGET).nacp --icon=icon.jpg

clean:
	rm -f *.elf *.nro *.nacp
