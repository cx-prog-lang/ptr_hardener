test:
	@rm -rf .tb || true
	@cp -r testbench .tb
	@cd .tb && clang main.c test/*.c --include ../ptr_hardener_rt.c -o run_test && ./run_test || true
	@rm -rf .tb || true
