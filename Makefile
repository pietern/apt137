DEFS=-D_POSIX_C_SOURCE=200809
CFLAGS=-std=c99 -Wall -g -ggdb $(DEFS)
LDFLAGS=-lm
MAIN=apt137
OBJS=main.o channel.o decoder.o

.PHONY: all
all: apt137

.PHONY: clean
clean:
	rm -f $(MAIN) $(OBJS)

.PHONY: dep
dep:
	cc -MM $(OBJS:.o=.c) > Makefile.dep

include Makefile.dep

$(MAIN): $(OBJS)
	cc $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c %.h
	cc $(CFLAGS) -c -o $@ $<
