test:
	@rm -rf .tb || true
	@mkdir .tb && cp -r * .tb
	@cd .tb && \
		clang --std=c23 -O3 -DNDEBUG -g basic.c \
			--include ../../ptr_hardener_rt.c -c -emit-llvm -o basic.bc && \
			llvm-dis basic.bc && \
		clang --std=c23 -O0 -g *.c \
			--include ../../ptr_hardener_rt.c -o run_test && \
		./run_test || true
