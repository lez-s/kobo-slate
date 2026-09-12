CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
LDFLAGS ?=

.PHONY: all test sanitize clean arm

all: test

test: test_core test_gesture
	./test_core
	./test_gesture

test_core: test_core.c store.c canvas.c notepad.h
	$(CC) $(CFLAGS) -o $@ test_core.c store.c canvas.c $(LDFLAGS) -lm

test_gesture: test_gesture.c gesture.c gesture.h
	$(CC) $(CFLAGS) -o $@ test_gesture.c gesture.c $(LDLIBS)

sanitize: test_core_san test_gesture_san
	./test_core_san
	./test_gesture_san

test_core_san: test_core.c store.c canvas.c notepad.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $@ test_core.c store.c canvas.c $(LDFLAGS) -lm

test_gesture_san: test_gesture.c gesture.c gesture.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $@ test_gesture.c gesture.c $(LDLIBS)

arm:
	./build.sh

clean:
	rm -f test_core test_gesture test_core_san test_gesture_san
