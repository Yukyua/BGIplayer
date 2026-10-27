CXX ?= clang++
SDL_CONFIG ?= sdl2-config
FFMPEG_PKGS := libavformat libavcodec libavutil libswresample libswscale

CXXFLAGS ?= -std=c++20 -O2 -g
CXXFLAGS += -Wall -Wextra -Wpedantic -Wconversion -Wshadow
CXXFLAGS += -MMD -MP
CXXFLAGS += $(shell $(SDL_CONFIG) --cflags)
CXXFLAGS += $(shell pkg-config --cflags $(FFMPEG_PKGS))
CXXFLAGS += $(shell pkg-config --cflags freetype2)
CPPFLAGS += -Iinclude
LDLIBS += $(filter-out -lSDL2main,$(shell $(SDL_CONFIG) --libs))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_PKGS))
LDLIBS += $(shell pkg-config --libs freetype2)
LDLIBS += -lm -lz
ifeq ($(shell uname -s),Darwin)
LDLIBS += -liconv
LDLIBS += -framework CoreText -framework CoreFoundation -framework Metal -framework Foundation -framework AppKit -framework Carbon
endif

BUILD := build
SRC := $(wildcard src/*.cpp)
OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRC))
ifeq ($(shell uname -s),Darwin)
OBJ += $(patsubst src/%.mm,$(BUILD)/%.o,$(wildcard src/*.mm))
endif
DEP := $(OBJ:.o=.d)
.PHONY: all clean

all: $(BUILD)/bgiplayer

$(BUILD)/bgiplayer: $(OBJ)
	$(CXX) $^ $(LDLIBS) -o $@

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(DEP)
