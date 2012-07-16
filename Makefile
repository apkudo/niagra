WAF=node-waf

build:
	@$(WAF) configure build

clean:
	@$(WAF) distclean

.PHONY: build distclean