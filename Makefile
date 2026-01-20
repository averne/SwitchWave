ifeq ($(strip $(DEVKITPRO)),)
    $(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

APP_TITLE               :=  SwitchWave
APP_AUTHOR              :=  averne
APP_ICON                :=  icon.jpg
APP_VERSION             :=  1.0.1
APP_COMMIT              :=  $(shell git rev-parse --short HEAD)

FFMPEG_CONFIG           :=  --enable-asm \
                            --enable-gpl \
                            --enable-nvtegra \
                            --enable-static --disable-shared \
							--enable-libdav1d --enable-libwebp \
                            --enable-zlib --enable-bzlib \
							--enable-libass --enable-libfreetype --enable-libfribidi \
                            --disable-doc --disable-programs \
							--disable-encoders --enable-encoder=mjpeg,png,libwebp \
							--disable-muxers \
							--target-os=horizon --enable-cross-compile \
                            --cross-prefix=aarch64-none-elf- --arch=aarch64 --cpu=cortex-a57 --enable-neon \
                            --enable-pic --disable-autodetect --disable-runtime-cpudetect --disable-debug

MPV_CONFIG              :=  --enable-libmpv-static --disable-libmpv-shared --disable-manpage-build \
                            --disable-cplayer --disable-iconv --disable-lua \
							--disable-sdl2 --disable-gl --disable-plain-gl --enable-hos-audio --enable-deko3d

TOPDIR                  ?=  $(CURDIR)

TARGET                  :=  SwitchWave
INCLUDES                :=  include src src/imgui src/imgui_impl_hos src/implot src/inih
SOURCES                 :=  src src/fs src/ui src/imgui src/imgui/misc/freetype src/imgui_impl_hos src/implot src/inih
SHADERS                 :=  src/shaders
TEXTURES                :=  assets/textures
BUILD                   :=  build
ROMFS                   :=  $(BUILD)/romfs
INSTALL                 :=  $(TOPDIR)/$(BUILD)/install
PACKAGES                :=  mpv libavcodec libavformat libavfilter libavutil libswscale libswresample \
                            uam libsmb2 libnfs libssh2 freetype2 libarchive
LIBDIRS                 :=  $(INSTALL)

DEFINES                 :=  __SWITCH__ _GNU_SOURCE _POSIX_VERSION=200809L timegm=mktime \
                            APP_TITLE=\"$(APP_TITLE)\" APP_VERSION=\"$(APP_VERSION)-$(shell git rev-parse --short HEAD)\" \
							IMGUI_ENABLE_FREETYPE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
ARCH                    :=  -march=armv8-a -mtune=cortex-a57 -mtp=soft -fpie -fPIC
FLAGS                   :=  -O2 -g -Wall -Wextra -pipe -ffunction-sections -fdata-sections \
                            -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS                  :=  -std=gnu11
CXXFLAGS                :=  -std=gnu++23
ASFLAGS                 :=
LDFLAGS                 :=  -g -Wl,--gc-sections -Wl,-pie -specs=$(DEVKITPRO)/libnx/switch.specs
LINKS                   :=  -lusbhsfs -lntfs-3g -llwext4 -ldeko3d -lnx
PREFIX                  :=  aarch64-none-elf-

# -----------------------------------------------
PORTLIBS                :=  $(DEVKITPRO)/portlibs/switch
LIBNX                   :=  $(DEVKITPRO)/libnx
LIBDIRS                 +=  $(PORTLIBS) $(LIBNX)

export PATH             :=  $(DEVKITPRO)/tools/bin:$(DEVKITPRO)/devkitA64/bin:$(PORTLIBS)/bin:$(PATH)
export PKG_CONFIG_PATH  :=  $(INSTALL)/lib/pkgconfig:$(PORTLIBS)/lib/pkgconfig
export PKG_CONFIG_LIBDIR =

export CC               :=  $(CCACHE) $(PREFIX)gcc
export CXX              :=  $(CCACHE) $(PREFIX)g++
export AS               :=  $(PREFIX)as
export AR               :=  $(PREFIX)ar
export LD               :=  $(PREFIX)g++
export NM               :=  $(PREFIX)nm
export PKG_CONFIG       :=  pkg-config
export SHELL            :=  env PATH=$(PATH) PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(SHELL)

OUTPUT                  :=  $(BUILD)/$(TARGET).nro
ELF_TARGET              :=  $(BUILD)/$(TARGET)
NACP_TARGET             :=  $(OUTPUT:.nro=.nacp)

DIST_FOLDER             :=  $(BUILD)/dist/switch/$(APP_TITLE)
DIST_TARGET             :=  $(BUILD)/$(APP_TITLE)-$(APP_VERSION)-$(APP_COMMIT).zip

CFILES                  :=  $(shell find $(SOURCES) -maxdepth 1 -name '*.c')
CPPFILES                :=  $(shell find $(SOURCES) -maxdepth 1 -name '*.cpp')
SFILES                  :=  $(shell find $(SOURCES) -maxdepth 1 -name '*.s' -or -name '*.S')
GLSLFILES               :=  $(notdir $(shell find $(SHADERS) -maxdepth 1 -name '*.glsl'))
SVGFILES                :=  $(notdir $(shell find $(TEXTURES) -maxdepth 1 -name '*.svg'))

OFILES                  :=  $(CFILES:%=$(BUILD)/%.o) $(CPPFILES:%=$(BUILD)/%.o) $(SFILES:%=$(BUILD)/%.o)
DFILES                  :=  $(OFILES:.o=.d)
DKSHFILES               :=  $(GLSLFILES:%.glsl=$(ROMFS)/shaders/%.dksh)
BCFILES                 :=  $(SVGFILES:%.svg=$(ROMFS)/textures/%.bc)

DEFINES_FLAGS           :=  $(addprefix -D,$(DEFINES))
INCLUDE_FLAGS           :=  $(addprefix -I,$(INCLUDES)) $(foreach dir,$(LIBDIRS),-I$(dir)/include)
LIB_FLAGS               :=

FLAGS                   :=  $(shell pkg-config --cflags $(PACKAGES)) $(FLAGS)
CFLAGS                  :=  $(DEFINES_FLAGS) $(INCLUDE_FLAGS) $(ARCH) $(FLAGS) $(CFLAGS)
CXXFLAGS                :=  $(DEFINES_FLAGS) $(INCLUDE_FLAGS) $(ARCH) $(FLAGS) $(CXXFLAGS)
LDFLAGS                 :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib) $(ARCH) $(LDFLAGS) $(LINKS)
LIB_WARNINGS            :=  -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-parameter \
                            -Wno-deprecated-declarations

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
    ROMFS_TARGET        :=  $(shell find $(ROMFS) -type 'f') $(DKSHFILES) $(BCFILES)
endif

# -----------------------------------------------
.SUFFIXES:
.PHONY: all dist clean mrproper \
        configure configure-ffmpeg configure-mpv configure-uam \
        libraries build-ffmpeg build-mpv build-uam
.PRECIOUS: %.nacp

all: $(OUTPUT)

dist: $(DIST_TARGET)

configure: configure-ffmpeg configure-mpv configure-uam
libraries: build-ffmpeg build-mpv build-uam

configure-ffmpeg:
	@echo Configuring ffmpeg with flags $(FFMPEG_CONFIG)
	@mkdir -p $(BUILD)/ffmpeg
	@cd $(BUILD)/ffmpeg; \
		$(TOPDIR)/ffmpeg/configure --prefix=$(INSTALL) \
		--cc="$(CC)" --cxx="$(CXX)" --ar="$(AR)" --nm="$(NM)" \
		--extra-cflags="-isystem $(LIBNX)/include $(LIB_WARNINGS)" \
		$(FFMPEG_CONFIG)

configure-mpv:
	@mkdir -p $(BUILD)/mpv
	@cd $(TOPDIR)/mpv; \
		./bootstrap.py
	@cd $(TOPDIR)/mpv; \
		CFLAGS="-isystem $(LIBNX)/include $(CFLAGS) $(LIB_WARNINGS)" \
		./waf configure -o $(TOPDIR)/$(BUILD)/mpv --prefix=$(INSTALL) $(MPV_CONFIG)
	@sed -i 's/#define HAVE_POSIX 1/#define HAVE_POSIX 0/' $(BUILD)/mpv/config.h

configure-uam:
	@cd $(TOPDIR)/libuam; \
		meson setup $(TOPDIR)/$(BUILD)/libuam --cross-file=$(TOPDIR)/misc/crossfile.txt --prefix=$(INSTALL)

build-ffmpeg:
	@$(MAKE) --no-print-directory -C $(BUILD)/ffmpeg install

build-mpv:
	@cd $(BUILD)/mpv; \
		$(TOPDIR)/mpv/waf install

build-uam:
	@meson install -C $(BUILD)/libuam

%.nro: % $(ROMFS_TARGET) $(DKSHFILES) $(BCFILES) $(APP_ICON) $(NACP_TARGET)
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

$(ROMFS)/shaders/%_vsh.dksh: $(SHADERS)/%_vsh.glsl
	@echo "VERT    " $@
	@mkdir -p $(dir $@)
	@uam -s vert -o $@ $<

$(ROMFS)/shaders/%_fsh.dksh: $(SHADERS)/%_fsh.glsl
	@echo "FRAG    " $@
	@mkdir -p $(dir $@)
	@uam -s frag -o $@ $<

$(ROMFS)/textures/%.bc: $(TEXTURES)/%.svg
	@echo "BCn     " $@
	@mkdir -p $(dir $@)
	@misc/gimp-bcn-convert.sh $< $@ > /dev/null 2>&1

run: $(OUTPUT)
	@nxlink -r 100 -s $(OUTPUT) -p SwitchWave/SwitchWave.nro

$(DIST_TARGET): $(OUTPUT)
	@rm -rf $(BUILD)/$(APP_TITLE)-*-*.zip
	@mkdir -p $(DIST_FOLDER)
	@cp $< $(DIST_FOLDER)
	@cp misc/mpv.conf $(DIST_FOLDER)
	@cd $(BUILD)/dist; zip -r $(TOPDIR)/$@ . >/dev/null; cd $(TOPDIR)
	@rm -rf $(BUILD)/dist
	@echo Compressed release to $@

clean:
	@echo Cleaning...
	@rm -rf $(OUTPUT) $(DIST_TARGET) $(ELF_TARGET) $(NACP_TARGET) $(addprefix $(BUILD)/,$(SOURCES)) $(DKSHFILES) $(BCFILES)

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
