SRC := src/node-launcherd.c src/str.c
HEADERS := src/str.h

all: out/node-launcherd

out:
	mkdir $@

out/node-launcherd: $(SRC) $(HEADERS) Makefile out
	$(CC) -Wall -Werror $(SRC) -o $@

.PHONY: clean
clean:
	rm -fr out

