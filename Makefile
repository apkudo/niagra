GYP=node-gyp

build:
	@$(GYP) configure build

clean:
	@$(GYP) clean

.PHONY: rebuild