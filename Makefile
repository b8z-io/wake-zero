# Builds wake for the CardputerZero (on the device, or in CI for arm64), or for a desktop to test headless.
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
# OpenMP spreads Ink's fluid simulation over the four cores; kept apart from CFLAGS so overriding those keeps it.
OMP = -fopenmp
LDLIBS = -lEGL -lGLESv2 -lgbm -lm
SRC = src/main.c src/gfx.c src/scenes.c src/analysis.c src/camera.c src/font.c src/capture.c src/ink.c

wake: $(SRC) src/*.h
	$(CC) $(CFLAGS) $(OMP) -o $@ $(SRC) $(LDLIBS) $(OMP)

clean:
	rm -f wake

.PHONY: clean
