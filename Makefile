PLATFORM ?= WII
DEBUG ?= false
USER_CFLAGS ?=

ifneq ($(PLATFORM),WII)
$(error Unknown PLATFORM '$(PLATFORM)'; only WII is supported)
endif

WII_BUILD_DIR = build/wii
WII_BUILD_OBJ = $(WII_BUILD_DIR)/obj
WII_TARGET_ELF = $(WII_BUILD_DIR)/boot.elf
WII_TARGET_DOL = $(WII_BUILD_DIR)/boot.dol
WII_CC ?= $(DEVKITPRO)/devkitPPC/bin/powerpc-eabi-gcc
WII_ELF2DOL ?= $(DEVKITPRO)/tools/bin/elf2dol
WII_PROFILE ?= 0
WII_PROFILE_FRAMES ?= 0
WII_PROFILE_WINDOW ?=
WII_SMOKE ?= 0
WII_SMOKE_INTRO ?= 0
WII_SMOKE_FULL ?= 0
WII_SMOKE_2P ?= 0
WII_DEBUG ?= 0
WII_DIST_DIR = dist
ifeq ($(WII_SMOKE_FULL),1)
WII_BUILD_DIR = build/wii-smoke-full
else ifeq ($(WII_SMOKE_INTRO),1)
WII_BUILD_DIR = build/wii-smoke-intro
else
ifeq ($(WII_SMOKE),1)
WII_BUILD_DIR = build/wii-smoke
endif
endif
ifeq ($(WII_PROFILE),1)
WII_BUILD_DIR := $(WII_BUILD_DIR)-profile
endif
ifeq ($(WII_DEBUG),1)
WII_BUILD_DIR := $(WII_BUILD_DIR)-debug
WII_DIST_DIR = dist-debug
endif

.DEFAULT_GOAL := $(WII_TARGET_DOL)

C_FLAGS ?= -Isrc/libs/ -std=gnu99 -Wall -Wno-unused-variable $(USER_CFLAGS)

ifeq ($(DEBUG), true)
	C_FLAGS := $(C_FLAGS) -g -DVERBOSE_PRINTING
else
	C_FLAGS := $(C_FLAGS) -O3
endif


# Renderer ---------------------------------------------------------------------

RENDERER_SRC = src/render_gx.c
C_FLAGS := $(C_FLAGS) -DRENDERER_GX


# Wii flags --------------------------------------------------------------------

WII_ARCH_FLAGS = -mrvl -mcpu=750 -meabi -mhard-float
C_FLAGS := $(C_FLAGS) -I$(DEVKITPRO)/libogc/include $(WII_ARCH_FLAGS) -DPLATFORM_WII
ifeq ($(WII_PROFILE),1)
	C_FLAGS := $(C_FLAGS) -DWII_PROFILE
ifneq ($(WII_PROFILE_FRAMES),0)
	C_FLAGS := $(C_FLAGS) -DWII_PROFILE_FRAMES=$(WII_PROFILE_FRAMES)
endif
ifneq ($(WII_PROFILE_WINDOW),)
	C_FLAGS := $(C_FLAGS) -DWII_PROFILE_WINDOW=$(WII_PROFILE_WINDOW)
endif
endif
WII_L_FLAGS = -L$(DEVKITPRO)/libogc/lib/wii -lwiiuse -lbte -lfat -lasnd -logc -lm

ifeq ($(WII_SMOKE_FULL),1)
	C_FLAGS := $(C_FLAGS) -DWII_SMOKE -DWII_SMOKE_FULL
else ifeq ($(WII_SMOKE_INTRO),1)
	C_FLAGS := $(C_FLAGS) -DWII_SMOKE -DWII_SMOKE_INTRO
else ifeq ($(WII_SMOKE),1)
	C_FLAGS := $(C_FLAGS) -DWII_SMOKE
endif
ifeq ($(WII_SMOKE_2P),1)
	C_FLAGS := $(C_FLAGS) -DWII_SMOKE_2P
endif
ifeq ($(WII_DEBUG),1)
	C_FLAGS := $(C_FLAGS) -DWII_DEBUG
endif


# Source files -----------------------------------------------------------------

COMMON_SRC = \
	src/wipeout/map.c \
	src/wipeout/race.c \
	src/wipeout/camera.c \
	src/wipeout/object.c \
	src/wipeout/droid.c \
	src/wipeout/ui.c \
	src/wipeout/hud.c \
	src/wipeout/image.c \
	src/wipeout/game.c \
	src/wipeout/menu.c \
	src/wipeout/main_menu.c \
	src/wipeout/ingame_menus.c \
	src/wipeout/title.c \
	src/wipeout/intro.c \
	src/wipeout/scene.c \
	src/wipeout/ship.c \
	src/wipeout/ship_ai.c \
	src/wipeout/ship_player.c \
	src/wipeout/track.c \
	src/wipeout/weapon.c \
	src/wipeout/particle.c \
	src/wipeout/sfx.c \
	src/wipeout/rumble.c \
	src/utils.c \
	src/types.c \
	src/system.c \
	src/mem.c \
	src/input.c \
	src/wipeout/cJSON.c \
	$(RENDERER_SRC)


# Targets Wii ------------------------------------------------------------------

WII_COMMON_OBJ = $(patsubst %.c,$(WII_BUILD_OBJ)/%.o,$(COMMON_SRC))
WII_COMMON_DEPS = $(patsubst %.c,$(WII_BUILD_OBJ)/%.d,$(COMMON_SRC))
WII_PLATFORM_OBJ = $(WII_BUILD_OBJ)/src/platform_wii.o

$(WII_TARGET_DOL): $(WII_TARGET_ELF)
	$(WII_ELF2DOL) $< $@

$(WII_TARGET_ELF): $(WII_PLATFORM_OBJ) $(WII_COMMON_OBJ)
	mkdir -p $(dir $@)
	$(WII_CC) $(WII_ARCH_FLAGS) $^ -o $@ $(WII_L_FLAGS)

$(WII_BUILD_OBJ)/%.o: %.c
	mkdir -p $(dir $@)
	$(WII_CC) $(C_FLAGS) -MMD -MP -c $< -o $@

-include $(WII_COMMON_DEPS) $(WII_BUILD_OBJ)/src/platform_wii.d

dist: $(WII_TARGET_DOL)
	mkdir -p $(WII_DIST_DIR)/apps/wiipeout/wipeout
	cp $(WII_TARGET_DOL) packaging/wii/meta.xml packaging/wii/icon.png $(WII_DIST_DIR)/apps/wiipeout/

.PHONY: dist clean
clean:
	$(RM) -rf $(WII_BUILD_DIR)
