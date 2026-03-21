test:
	@rm -rf .tb || true
	@cp -r testbench .tb
	@cp ptr_hardener_rt.c .tb
	@cd .tb && clang main.c test/*.c ptr_hardener_rt.c -o run_test && ./run_test || true
	@rm -rf .tb || true
