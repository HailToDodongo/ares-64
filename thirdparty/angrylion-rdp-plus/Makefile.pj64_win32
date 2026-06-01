BUILD_DIR := build/pj64-win32
TARGET := $(BUILD_DIR)/angrylion-plus.dll
INSTALL_DIR := $(PJ64_DIR)/Plugin/GFX
WINSDK_PATH := "/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/SDK/ScopeCppSDK/vc15/SDK/include"

CC := clang.exe
CXX := clang++.exe

WINDRES := llvm-windres-18
WINDRES_FLAGS := --target=i386-windows-pc
WINDRES_INC := --include-dir=$(WINSDK_PATH)/um --include-dir=$(WINSDK_PATH)/shared

CLANG_FORMAT := clang-format-14
FORMAT_ARGS := -i -style=file
FORMAT_FILES := $(shell find src -type f -name "*.[ch]") $(shell find src -type f -name "*.cpp")

OPTFLAGS := -march=x86-64-v2 -O3 -ffast-math -flto
CFLAGS   := -x c   -fno-PIC -std=gnu17 -m32 -target i386-windows-pc -fvisibility=hidden
CXXFLAGS := -x c++ -fno-PIC -std=c++20 -m32 -target i386-windows-pc -fvisibility=hidden -fvisibility-inlines-hidden
WARNFLAGS := -Wall -Wextra -Wshadow -Wpointer-arith -Wformat=2 -Wnull-dereference -Woverflow -Wimplicit-fallthrough
# WARNFLAGS += -Weverything -Wno-cast-function-type-strict -Wno-reserved-macro-identifier -Wno-unsafe-buffer-usage
# WARNFLAGS += -Wno-declaration-after-statement -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-exit-time-destructors
# WARNFLAGS += -Wno-global-constructors -Wno-date-time -Wno-unused-macros -Wno-sign-conversion -Wno-cast-align -Wno-implicit-int-conversion
WARNFLAGS += -Werror=implicit-int -Werror=implicit-function-declaration -Werror=int-conversion -Werror=incompatible-pointer-types -Werror=return-type
DEFS := -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS
INCLUDES := -Isrc
DEPFLAGS = -MMD -MP -MF $(@:.o=.d)

LDFLAGS := -fuse-ld=lld-link -shared $(OPTFLAGS) -m32 -target i386-windows-pc -Wl,/machine:x86
LDLIBS := -luser32 -lshlwapi -lopengl32 -lgdi32 -lcomctl32 -lmsvcrt

CORE_DIRS      := $(shell find src/core -type d -not -path "src/core/n64video*")
CORE_C_FILES   := $(foreach dir, $(CORE_DIRS), $(wildcard $(dir)/*.c))
CORE_CXX_FILES := $(foreach dir, $(CORE_DIRS), $(wildcard $(dir)/*.cpp))
CORE_O_FILES   := $(foreach f, $(CORE_C_FILES), $(BUILD_DIR)/$(f:.c=.o)) \
                  $(foreach f, $(CORE_CXX_FILES), $(BUILD_DIR)/$(f:.cpp=.o))

OUTPUT_DIRS      := $(shell find src/output -type d)
OUTPUT_C_FILES   := $(foreach dir, $(OUTPUT_DIRS), $(wildcard $(dir)/*.c))
OUTPUT_CXX_FILES := $(foreach dir, $(OUTPUT_DIRS), $(wildcard $(dir)/*.cpp))
OUTPUT_O_FILES   := $(foreach f, $(OUTPUT_C_FILES), $(BUILD_DIR)/$(f:.c=.o)) \
                    $(foreach f, $(OUTPUT_CXX_FILES), $(BUILD_DIR)/$(f:.cpp=.o))

PJ64_DIRS      := $(shell find src/plugin/zilmar -type d)
PJ64_C_FILES   := $(foreach dir, $(PJ64_DIRS), $(wildcard $(dir)/*.c))
PJ64_CXX_FILES := $(foreach dir, $(PJ64_DIRS), $(wildcard $(dir)/*.cpp))
PJ64_RC_FILES  := $(foreach dir, $(PJ64_DIRS), $(wildcard $(dir)/*.rc))
PJ64_O_FILES   := $(foreach f, $(PJ64_C_FILES), $(BUILD_DIR)/$(f:.c=.o)) \
                  $(foreach f, $(PJ64_CXX_FILES), $(BUILD_DIR)/$(f:.cpp=.o)) \
                  $(foreach f, $(PJ64_RC_FILES), $(BUILD_DIR)/$(f:.rc=.rc.o))

DEP_FILES := $(CORE_O_FILES:.o=.d) $(OUTPUT_O_FILES:.o=.d) $(PJ64_O_FILES:.o=.d)

# fixpaths is a dreadful hack for converting windows paths to unix paths in dep files output by clang++.exe
$(shell python3 tools/fixpaths.py $(BUILD_DIR))
$(shell mkdir -p $(BUILD_DIR) $(foreach dir, $(CORE_DIRS) $(OUTPUT_DIRS) $(PJ64_DIRS), $(BUILD_DIR)/$(dir)))

.PHONY: all clean format install

all: $(TARGET)

clean:
	$(RM) -r $(BUILD_DIR)

format:
	$(CLANG_FORMAT) $(FORMAT_ARGS) $(FORMAT_FILES)
# Trim trailing whitespace
	$(foreach f,$(FORMAT_FILES),$(shell sed -i 's/[ \t]*$$//' $f))
# Add missing newlines
	$(foreach f,$(FORMAT_FILES),$(shell [ -n "$$(tail -c1 $f)" ] && printf '\n' >> $f))

install: all
	cp $(TARGET) $(INSTALL_DIR)

$(TARGET): $(CORE_O_FILES) $(OUTPUT_O_FILES) $(PJ64_O_FILES)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/src/%.o: src/%.c
	$(CC) $(CFLAGS) $(OPTFLAGS) $(INCLUDES) $(DEPFLAGS) $(WARNFLAGS) $(DEFS) -c $< -o $@

$(BUILD_DIR)/src/%.o: src/%.cpp
	$(CC) $(CXXFLAGS) $(OPTFLAGS) $(INCLUDES) $(DEPFLAGS) $(WARNFLAGS) $(DEFS) -c $< -o $@

$(BUILD_DIR)/src/%.rc.o: src/%.rc
	$(WINDRES) $(WINDRES_FLAGS) $(WINDRES_INC) $< -o $@

# Dependencies

-include $(DEP_FILES)
