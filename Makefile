# Builds wake for the CardputerZero (on the device, or in CI for arm64), or for a desktop to test headless.
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS = -lEGL -lGLESv2 -lgbm -lm
SRC = src/main.c src/gfx.c src/scenes.c src/analysis.c src/camera.c src/font.c src/capture.c

wake: $(SRC) src/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f wake

.PHONY: clean
