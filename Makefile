CFLAGS=-Wall -g -ggdb
LDFLAGS=-lm
MAIN=apt137
OBJS=main.o channel.o decoder.o

.PHONY: all
all: apt137

.PHONY: clean
clean:
	rm -f $(MAIN) $(OBJS)

$(MAIN): $(OBJS)
	cc $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c %.h
	cc $(CFLAGS) -c -o $@ $<
