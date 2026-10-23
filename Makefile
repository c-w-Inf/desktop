RAYLIB_DIR ?= third_party/raylib
JSON_C_CFLAGS := $(shell pkg-config --cflags json-c 2>/dev/null)
JSON_C_LIBS   := $(shell pkg-config --libs json-c 2>/dev/null)
CC         ?= cc
CFLAGS     ?= -O2
CFLAGS     += -std=c99 -Wall -Wextra -Isrc -Ithird_party -I$(RAYLIB_DIR)/include $(JSON_C_CFLAGS)
LDFLAGS    += -L$(RAYLIB_DIR)/lib -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
LDFLAGS    += -lminizip -lz
LDFLAGS    += $(if $(JSON_C_LIBS),$(JSON_C_LIBS),-ljson-c)
LDFLAGS    += -Wl,-rpath,'$$ORIGIN/$(RAYLIB_DIR)/lib'

TARGET  := image_canvas
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,obj/%.o,$(SRC))
HEADERS := $(wildcard src/*.h) $(wildcard third_party/*.h)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

obj/%.o: src/%.c $(HEADERS) | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj:
	mkdir -p obj

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
	rm -rf obj
