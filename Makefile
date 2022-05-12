# Build unixtool

CC     ?= cc
RM     ?= rm -f
STRIP  ?= strip
CFLAGS ?= -Os -Wall -Wextra -pedantic

.PHONY: all
unixtool: unixtool.c

.PHONY: clean
clean:
	-$(RM) -f ./unixtool

.PHONY: strip
strip: unixtool
	-$(STRIP) ./unixtool
