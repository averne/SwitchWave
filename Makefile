ifeq ($(strip $(HOST)),)
    $(error Needs a host (hos/linux))
endif

TOPDIR                  ?=  $(CURDIR)

TARGET                  :=  AmpNX
INCLUDES                :=  include
SOURCES                 :=  src
ROMFS                   :=  assets
BUILD                   :=  build-$(HOST)
INSTALL                 :=  $(TOPDIR)/build-$(HOST)/install
PACKAGES                :=  mpv libavcodec libavdevice libavformat libavfilter libavutil libswscale libswresample
LIBDIRS                 :=  $(INSTALL)

APP_TITLE               :=  AmpNX
APP_AUTHOR              :=  averne
APP_ICON                :=
APP_VERSION             :=

FFMPEG_CONFIG           :=  --enable-asm \
						    --enable-network \
                            --enable-gpl \
                            --enable-tx1 \
                            --enable-static --disable-shared \
                            --enable-zlib --enable-bzlib --enable-libass --enable-libfreetype --enable-libfribidi \
                            --disable-doc \
                            --disable-programs
MPV_CONFIG              :=  --enable-libmpv-static --disable-libmpv-shared \
                            --disable-cplayer --disable-iconv --disable-jpeg

ifeq ($(strip $(HOST)),hos)

PACKAGES                +=  uam

FFMPEG_CONFIG           +=  --target-os=horizon --enable-cross-compile \
                            --cross-prefix=aarch64-none-elf- --arch=aarch64 --cpu=cortex-a57 --enable-neon \
							--enable-pic --disable-autodetect --disable-runtime-cpudetect --disable-debug
MPV_CONFIG              +=  --disable-sdl2 --disable-gl --disable-plain-gl --enable-hos-audio --enable-deko3d

DEFINES                 :=  __SWITCH__ _GNU_SOURCE _POSIX_VERSION=200809L
ARCH                    :=  -march=armv8-a+crc+crypto+simd -mtune=cortex-a57 -mtp=soft -fpie
FLAGS                   :=  -O2 -g -Wall -Wextra -pipe -ffunction-sections -fdata-sections \
                            -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS                  :=  -std=gnu11
CXXFLAGS                :=  -std=gnu++20
ASFLAGS                 :=
LDFLAGS                 :=  -g -Wl,--gc-sections -Wl,-pie -specs=$(DEVKITPRO)/libnx/switch.specs
LINKS                   :=  -lnx
PREFIX                  :=  aarch64-none-elf-

else ifeq ($(strip $(HOST)),linux)

FFMPEG_CONFIG           +=  --disable-optimizations --disable-small

DEFINES                 :=
ARCH                    :=  -march=native
FLAGS                   :=  -g -ggdb -Wall -Wextra -pipe -O0 -ffunction-sections -fdata-sections
CFLAGS                  :=  -std=gnu11
CXXFLAGS                :=  -std=gnu++20 -fno-rtti -fno-exceptions
ASFLAGS                 :=
LDFLAGS                 :=  -g -Wl,--gc-sections
LINKS                   :=
PREFIX                  :=
CCACHE                  :=  ccache
else
    $(error Wrong host $(HOST), need hos/linux)
endif

# -----------------------------------------------
ifeq ($(strip $(HOST)),hos)

ifeq ($(strip $(DEVKITPRO)),)
    $(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

PORTLIBS                :=  $(DEVKITPRO)/portlibs/switch
LIBNX                   :=  $(DEVKITPRO)/libnx
PKG_CONFIG_PATH         :=  $(PORTLIBS)/lib/pkgconfig
LIBDIRS                 :=  $(LIBDIRS) $(PORTLIBS) $(LIBNX)

export PATH             :=  $(DEVKITPRO)/tools/bin:$(DEVKITPRO)/devkitA64/bin:$(PORTLIBS)/bin:$(PATH)
export PKG_CONFIG_LIBDIR =

endif

export PKG_CONFIG_PATH  :=  $(INSTALL)/lib/pkgconfig:$(PKG_CONFIG_PATH)

export CC               :=  $(CCACHE) $(PREFIX)gcc
export CXX              :=  $(CCACHE) $(PREFIX)g++
export AS               :=  $(PREFIX)as
export AR               :=  $(PREFIX)ar
export LD               :=  $(PREFIX)g++
export NM               :=  $(PREFIX)nm
export PKG_CONFIG       :=  pkg-config
export SHELL            :=  env PATH=$(PATH) PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(SHELL)

ELF_TARGET              :=  $(BUILD)/$(TARGET)
ifeq ($(strip $(HOST)),hos)
OUTPUT                  :=  $(BUILD)/$(TARGET).nro
NACP_TARGET             :=  $(OUTPUT:.nro=.nacp)
else
OUTPUT                  :=  $(ELF_TARGET)
endif
CFILES                  :=  $(shell find $(SOURCES) -maxdepth 1 -name '*.c')
CPPFILES                :=  $(shell find $(SOURCES) -maxdepth 1 -name '*.cpp')
SFILES                  :=  $(shell find $(SOURCES) -maxdepth 1 -name '*.s' -or -name '*.S')

OFILES                  :=  $(CFILES:%=$(BUILD)/%.o) $(CPPFILES:%=$(BUILD)/%.o) $(SFILES:%=$(BUILD)/%.o)
DFILES                  :=  $(OFILES:.o=.d)

DEFINES_FLAGS           :=  $(addprefix -D,$(DEFINES))
INCLUDE_FLAGS           :=  $(addprefix -I,$(INCLUDES)) $(foreach dir,$(LIBDIRS),-I$(dir)/include)
LIB_FLAGS               :=

FLAGS                   :=  $(shell pkg-config --cflags $(PACKAGES)) $(FLAGS)
CFLAGS                  :=  $(DEFINES_FLAGS) $(INCLUDE_FLAGS) $(ARCH) $(FLAGS) $(CFLAGS)
CXXFLAGS                :=  $(DEFINES_FLAGS) $(INCLUDE_FLAGS) $(ARCH) $(FLAGS) $(CXXFLAGS)
LDFLAGS                 :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib) $(ARCH) $(LDFLAGS) $(LINKS)
LIB_WARNINGS            :=  -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-parameter \
                            -Wno-deprecated-declarations -Wno-declaration-after-statement -Wno-undef

export CFLAGS
export CXXFLAGS
export ARFLAGS
export ASFLAGS
export LDFLAGS

# -----------------------------------------------
ifeq ($(strip $(APP_TITLE)),)
    APP_TITLE           :=  $(TARGET)
endif

ifeq ($(strip $(APP_AUTHOR)),)
    APP_AUTHOR          :=  Unspecified
endif

ifeq ($(strip $(APP_VERSION)),)
    APP_VERSION         :=  Unspecified
endif

ifneq ($(APP_TITLEID),)
    NACPFLAGS           +=  --titleid=$(strip $(APP_TITLEID))
endif

ifeq ($(strip $(APP_ICON)),)
    APP_ICON            :=  $(LIBNX)/default_icon.jpg
endif

NROFLAGS                :=  --icon=$(strip $(APP_ICON)) --nacp=$(strip $(NACP_TARGET))

ifneq ($(ROMFS),)
    NROFLAGS            +=  --romfsdir=$(strip $(ROMFS))
    ROMFS_TARGET        +=  $(shell find $(ROMFS) -type 'f')
endif

# -----------------------------------------------
.SUFFIXES:
.PHONY: all clean mrproper \
        configure configure-ffmpeg configure-mpv configure-uam \
        libraries build-ffmpeg build-mpv build-uam
.PRECIOUS: %.nacp

all: $(OUTPUT)

configure: configure-ffmpeg configure-mpv configure-uam

configure-ffmpeg:
	@echo Configuring ffmpeg with flags $(FFMPEG_CONFIG)
	@mkdir -p $(BUILD)/ffmpeg
	@cd $(BUILD)/ffmpeg; \
		$(TOPDIR)/ffmpeg/configure --prefix=$(INSTALL) \
		--cc="$(CC)" --cxx="$(CXX)" --ar="$(AR)" --nm="$(NM)" \
		--extra-cflags="$(CFLAGS) $(LIB_WARNINGS)" \
		$(FFMPEG_CONFIG)

configure-mpv:
	@mkdir -p $(BUILD)/mpv
	@cd $(TOPDIR)/mpv; \
		./bootstrap.py
	@cd $(TOPDIR)/mpv; \
		CFLAGS="$(CFLAGS) $(LIB_WARNINGS)" \
		./waf configure -o $(TOPDIR)/$(BUILD)/mpv --prefix=$(INSTALL) $(MPV_CONFIG)
	@sed -i 's/#define HAVE_POSIX 1/#define HAVE_POSIX 0/' $(BUILD)/mpv/config.h

configure-uam:
	@cd $(TOPDIR)/libuam; \
		$(DEVKITPRO)/meson-toolchain.sh switch > crossfile.txt
	@cd $(TOPDIR)/libuam; \
		meson $(TOPDIR)/$(BUILD)/libuam --cross-file=crossfile.txt --prefix=$(INSTALL)

libraries: build-ffmpeg build-mpv build-uam

build-ffmpeg:
	@$(MAKE) --no-print-directory -C $(BUILD)/ffmpeg install

build-mpv:
	@cd $(BUILD)/mpv; \
		$(TOPDIR)/mpv/waf install

build-uam:
	@meson install -C $(BUILD)/libuam

%.nro: % $(ROMFS_TARGET) $(APP_ICON) $(NACP_TARGET)
	@echo "NRO     " $@
	@mkdir -p $(dir $@)
	@elf2nro $(ELF_TARGET) $@ $(NROFLAGS) > /dev/null
	@echo "Built" $(notdir $@)

%.nacp:
	@echo "NACP    " $@
	@mkdir -p $(dir $@)
	@nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@ $(NACPFLAGS)

$(ELF_TARGET): $(OFILES) $(wildcard $(INSTALL)/lib/*.a)
	@echo "LD      " $@
	@mkdir -p $(dir $@)
	@$(LD) $(OFILES) -o $@ $(shell pkg-config --libs --static $(PACKAGES)) $(LDFLAGS)
	@echo "Built" $(notdir $@)

$(BUILD)/%.c.o: %.c
	@echo "CC      " $@
	@mkdir -p $(dir $@)
	@$(CC) -MMD -MP $(CFLAGS) -c $(CURDIR)/$< -o $@

$(BUILD)/%.cpp.o: %.cpp
	@echo "CXX     " $@
	@mkdir -p $(dir $@)
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $(CURDIR)/$< -o $@

$(BUILD)/%.s.o: %.s %.S
	@echo "AS      " $@
	@mkdir -p $(dir $@)
	@$(AS) -MMD -MP -x assembler-with-cpp $(ARCH) $(RELEASE_FLAGS) $(RELEASE_ASFLAGS) $(INCLUDE_FLAGS) -c $(CURDIR)/$< -o $@

clean:
	@echo Cleaning...
	@rm -rf $(OUTPUT) $(addprefix $(BUILD)/,$(SOURCES))

mrproper: clean clean-ffmpeg clean-mpv clean-uam
	@rm -rf $(INSTALL)

clean-ffmpeg:
	@$(MAKE) --no-print-directory -C $(BUILD)/ffmpeg clean

clean-mpv:
	@cd $(BUILD)/mpv; \
		$(TOPDIR)/mpv/waf clean

clean-uam:
	@meson compile -C $(BUILD)/libuam --clean

-include $(DFILES)
