all: out/node-launcherd

out:
	mkdir $@

out/node-launcherd: src/node-launcherd.c out
	$(CC) -Wall -Werror $< -o $@

.PHONY: clean
clean:
	rm -fr out

