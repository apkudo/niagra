SRC := src/node-launcherd.c src/str.c

all: out/node-launcherd

out:
	mkdir $@

out/node-launcherd: $(SRC) out
	$(CC) -Wall -Werror $(SRC) -o $@

.PHONY: clean
clean:
	rm -fr out

