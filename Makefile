CC = cl65
AS = ca65
CFLAGS = -t c64 -O --standard c99
ASFLAGS = -t c64
LDFLAGS = -t c64 -m build/meshcore64.map

TARGET = build/meshcore64.prg
C_SOURCES = src/main.c src/screen.c src/serial.c src/meshcore.c src/input.c src/config.c
S_SOURCES = src/nmi_acia.s
C_OBJECTS = $(C_SOURCES:src/%.c=build/%.o)
S_OBJECTS = $(S_SOURCES:src/%.s=build/%.o)
OBJECTS = $(C_OBJECTS) $(S_OBJECTS)
HEADERS = $(wildcard src/*.h)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS) | build
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

build/%.o: src/%.c $(HEADERS) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: src/%.s | build
	$(AS) $(ASFLAGS) -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build

run: $(TARGET)
	./scripts/run.sh

.PHONY: vice
vice: $(TARGET)
	x64sc -autostart $(TARGET)
