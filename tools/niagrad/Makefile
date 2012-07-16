SRC := src/niagrad.c src/str.c
HEADERS := src/str.h

all: out/niagrad

out:
	mkdir $@

out/niagrad: $(SRC) $(HEADERS) Makefile out
	$(CC) -Wall -Werror $(SRC) -o $@

.PHONY: clean
clean:
	rm -fr out

