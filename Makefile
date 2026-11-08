CC ?= gcc
EMCC ?= emcc
PLATFORM ?= DESKTOP
UNAME_S := $(shell uname -s)
RENDERER ?= GL
USE_GLX ?= false
DEBUG ?= false
USER_CFLAGS ?=

WII_BUILD_DIR = build/wii
WII_BUILD_OBJ = $(WII_BUILD_DIR)/obj
WII_TARGET_ELF = $(WII_BUILD_DIR)/boot.elf
WII_TARGET_DOL = $(WII_BUILD_DIR)/boot.dol
WII_CC ?= $(DEVKITPRO)/devkitPPC/bin/powerpc-eabi-gcc
WII_ELF2DOL ?= $(DEVKITPRO)/tools/bin/elf2dol

ifeq ($(PLATFORM),WII)
	.DEFAULT_GOAL := $(WII_TARGET_DOL)
else
	.DEFAULT_GOAL := sdl
endif

L_FLAGS ?= -lm
C_FLAGS ?= -Isrc/libs/ -std=gnu99 -Wall -Wno-unused-variable $(USER_CFLAGS)

ifeq ($(DEBUG), true)
	C_FLAGS := $(C_FLAGS) -g -DVERBOSE_PRINTING
else
	C_FLAGS := $(C_FLAGS) -O3
endif


# Renderer ---------------------------------------------------------------------

ifeq ($(PLATFORM),WII)
	RENDERER_SRC = src/render_null.c
	C_FLAGS := $(C_FLAGS) -DRENDERER_NULL
else ifeq ($(RENDERER), GL)
	RENDERER_SRC = src/render_gl.c
	C_FLAGS := $(C_FLAGS) -DRENDERER_GL
	L_FLAGS := $(L_FLAGS)

else ifeq ($(RENDERER), SOFTWARE)
	RENDERER_SRC = src/render_software.c
	C_FLAGS := $(C_FLAGS) -DRENDERER_SOFTWARE
	L_FLAGS := $(L_FLAGS)
else ifeq ($(RENDERER), VK)
	RENDERER_SRC = src/render_vk.c
	C_FLAGS := $(C_FLAGS) -DRENDERER_VK
	L_FLAGS := $(L_FLAGS) -lvulkan
else ifeq ($(RENDERER), MTL)
	RENDERER_SRC = src/render_mtl.c
	C_FLAGS := $(C_FLAGS) -DRENDERER_MTL

else
$(error Unknown RENDERER)
endif

ifeq ($(GL_VERSION), GLES2)
	C_FLAGS := $(C_FLAGS) -DUSE_GLES2
endif



# macOS ------------------------------------------------------------------------

ifneq ($(PLATFORM),WII)
ifeq ($(UNAME_S), Darwin)
	BREW_HOME := $(shell brew --prefix)
	C_FLAGS := $(C_FLAGS) -x objective-c -I/opt/homebrew/include -D_THREAD_SAFE -w
	L_FLAGS := $(L_FLAGS) -L$(BREW_HOME)/lib -framework Foundation

    ifeq ($(RENDERER), GL)
        L_FLAGS := $(L_FLAGS) -lGLEW -GLU -framework OpenGL
    else ifeq ($(RENDERER), MTL)
        L_FLAGS := $(L_FLAGS) -framework Metal
    endif

	L_FLAGS_SDL = -lSDL2
	L_FLAGS_SDL3 = $(shell pkg-config --libs sdl3)
	L_FLAGS_SOKOL = -framework Cocoa -framework QuartzCore -framework AudioToolbox


# Linux ------------------------------------------------------------------------

else ifeq ($(UNAME_S), Linux)
	ifeq ($(RENDERER), GL)
		L_FLAGS := $(L_FLAGS) -lGLEW

		# Prefer modern GLVND instead of legacy X11-only GLX
		ifeq ($(USE_GLX), true)
			L_FLAGS := $(L_FLAGS) -lGL
		else
			L_FLAGS := $(L_FLAGS) -lOpenGL
		endif
	endif

	L_FLAGS_SDL = -lSDL2
	L_FLAGS_SDL3 = $(shell pkg-config --libs sdl3)
	L_FLAGS_SOKOL = -lX11 -lXcursor -pthread -lXi -ldl -lasound


# Windows MSYS ------------------------------------------------------------------
else ifeq ($(shell uname -o), Msys)
	ifeq ($(RENDERER), GL)
		L_FLAGS := $(L_FLAGS) -lglew32 -lopengl32
	endif

	C_FLAGS := $(C_FLAGS) -DSDL_MAIN_HANDLED
	L_FLAGS_SDL := $(shell sdl2-config --libs)
	L_FLAGS_SDL3 = $(shell pkg-config --libs sdl3)
	L_FLAGS_SOKOL = --pthread -lgdi32 -lole32


# Windows NON-MSYS ---------------------------------------------------------------
else ifeq ($(OS), Windows_NT)
	$(error TODO: FLAGS for windows have not been set up. Please modify this makefile and send a PR!)

else
$(error Unknown environment)
endif
else
	WII_ARCH_FLAGS = -mrvl -mcpu=750 -meabi -mhard-float
	C_FLAGS := $(C_FLAGS) -I$(DEVKITPRO)/libogc/include $(WII_ARCH_FLAGS) -DPLATFORM_WII
	WII_L_FLAGS = -L$(DEVKITPRO)/libogc/lib/wii -lwiiuse -lbte -lfat -logc -lm
endif



# Source files -----------------------------------------------------------------

TARGET_NATIVE ?= wipegame
BUILD_DIR = build/obj/native
BUILD_DIR_WASM = build/obj/wasm

WASM_RELEASE_DIR ?= build/wasm
TARGET_WASM ?= $(WASM_RELEASE_DIR)/wipeout.js
TARGET_WASM_MINIMAL ?= $(WASM_RELEASE_DIR)/wipeout-minimal.js

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
	src/utils.c \
	src/types.c \
	src/system.c \
	src/mem.c \
	src/input.c \
	src/wipeout/cJSON.c \
	$(RENDERER_SRC)


# Targets native ---------------------------------------------------------------

COMMON_OBJ = $(patsubst %.c, $(BUILD_DIR)/%.o, $(COMMON_SRC))
COMMON_DEPS = $(patsubst %.c, $(BUILD_DIR)/%.d, $(COMMON_SRC))

sdl: C_FLAGS += $(shell sdl2-config --cflags)
sdl: $(BUILD_DIR)/src/platform_sdl.o
sdl: $(COMMON_OBJ)
	$(CC) $^ -o $(TARGET_NATIVE) $(L_FLAGS) $(L_FLAGS_SDL)

sdl3: C_FLAGS += $(shell pkg-config --cflags sdl3)
sdl3: $(BUILD_DIR)/src/platform_sdl3.o
sdl3: $(COMMON_OBJ)
	$(CC) $^ -o $(TARGET_NATIVE) $(L_FLAGS) $(L_FLAGS_SDL3)

sokol: $(BUILD_DIR)/src/platform_sokol.o
sokol: $(COMMON_OBJ)
	$(CC) $^ -o $(TARGET_NATIVE) $(L_FLAGS) $(L_FLAGS_SOKOL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(C_FLAGS) -MMD -MP -c $< -o $@

-include $(COMMON_DEPS)


# Targets wasm -----------------------------------------------------------------

COMMON_OBJ_WASM = $(patsubst %.c, $(BUILD_DIR_WASM)/%.o, $(COMMON_SRC))
COMMON_DEPS_WASM = $(patsubst %.c, $(BUILD_DIR_WASM)/%.d, $(COMMON_SRC))

wasm: wasm_full wasm_minimal
	cp src/wasm-index.html $(WASM_RELEASE_DIR)/game.html


wasm_full: $(BUILD_DIR_WASM)/src/platform_sokol.o
wasm_full: $(COMMON_OBJ_WASM)
	mkdir -p $(WASM_RELEASE_DIR)
	$(EMCC) $^ -o $(TARGET_WASM) -lGLEW -lGL \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s ENVIRONMENT=web \
		--preload-file wipeout

wasm_minimal: $(BUILD_DIR_WASM)/src/platform_sokol.o
wasm_minimal: $(COMMON_OBJ_WASM)
	mkdir -p $(WASM_RELEASE_DIR)
	$(EMCC) $^ -o $(TARGET_WASM_MINIMAL) -lGLEW -lGL \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s ENVIRONMENT=web \
		--preload-file wipeout \
		--exclude-file wipeout/music \
		--exclude-file wipeout/intro.mpeg

$(BUILD_DIR_WASM):
	mkdir -p $(BUILD_DIR_WASM)

$(BUILD_DIR_WASM)/%.o: %.c
	mkdir -p $(dir $@)
	$(EMCC) $(C_FLAGS) -MMD -MP -c $< -o $@

-include $(COMMON_DEPS_WASM)


# Targets Wii ------------------------------------------------------------------

ifeq ($(PLATFORM),WII)
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
	mkdir -p dist/apps/wiipeout
	cp $(WII_TARGET_DOL) packaging/wii/meta.xml packaging/wii/icon.png dist/apps/wiipeout/

.PHONY: dist
endif



.PHONY: clean
clean:
	$(RM) -rf $(BUILD_DIR) $(BUILD_DIR_WASM) $(WASM_RELEASE_DIR) $(WII_BUILD_DIR)
