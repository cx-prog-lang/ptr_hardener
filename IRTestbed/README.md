# Ready-made LLVM IR ModulePass testbed

LLVM IR ModulePass testbed with zero boilerplate coding. Tested with LLVM 15. It
probably also works with later versions unless they drastically change their
APIs (again).

## Prerequisites

 - LLVM 15+ (either system-wide, source-code built, or
   [pre-built](https://releases.llvm.org/download.html))
 - CMake 3.22.1+ (moderately lower versions _may_ work, too)

## Setup

0. Download a [pre-built LLVM](https://releases.llvm.org/download.html), unpack it, and add a path to LLVM.

```
$ export PATH=<path/to/llvm/bin/dir>:$PATH    # Ignore this if you already have LLVM.
```

1. Clone the repository. 

```
$ git clone git@github.com:gwangmu/IRTestbed.git
$ cd IRTestbed
```

2. Make.

```
$ make
```

3. Check `build` for built binaries (`tb-clang`/`tb-clang++`).

## Usage

1. Implement a module pass in `testbed.cpp::IRTestbed::run()`.
2. Make again.
3. Use `tb-clang` and `tb-clang++` in `build` as C/C++ compilers.
    - The module pass is applied to compiled C/C++ source code.

## FAQ

* Can I move `tb-clang`/`tb-clang++` to another directory?
    - No, unfortunately, but you can always make symbolic links to them or add a
      path to `build` to the `PATH` environment variable.

## What you can get

* (Obviously) ready-to-use LLVM IR pass testbed.
* How to build an LLVM plugin with CMake. (-> CMakeFiles.txt)
* How to write a compiler wrapper with C. (-> wrapper.cpp)
