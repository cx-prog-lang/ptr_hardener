/*
 * Executable clang wrapper with the testbed.
 * 
 * Mostly based on AFL. (https://github.com/google/AFL)
 * See "llvm_mode/afl-clang-fast.c" for the original implementation.
 */

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
  char** cc_params = (char**)malloc((argc + 128) * sizeof(char*));
  int cc_par_cnt = 1;

  char _exepath[512];
  int _len = readlink("/proc/self/exe", _exepath, sizeof(_exepath));
  if (_len < 0) {
    std::cerr << "fatal: fail to resolve executable path.\n";
    abort();
  }

  std::string exepath = std::string(_exepath);

  int _name_pos = exepath.rfind('/');
  std::string exename = exepath.substr(_name_pos + 1); 
  std::string basepath = exepath.substr(0, _name_pos);

  std::string real_exepath;
  if (exename == "tb-clang++")
    real_exepath = basepath + "/clang++";
  else
    real_exepath = basepath + "/clang";
  cc_params[0] = (char*)real_exepath.c_str();

  std::string clang_chk = "which " + real_exepath + " >/dev/null 2>&1";
  if (system(clang_chk.c_str())) {
    std::cerr << "fatal: fail to find '" << real_exepath << "'.";
    abort();
  }

  std::string libpath = std::string(basepath + "/libIRTestbed.so");
  std::string pass_plugin = "-fpass-plugin=" + libpath;

  cc_params[cc_par_cnt++] = (char*)"-Xclang";
  cc_params[cc_par_cnt++] = (char*)pass_plugin.c_str();
  
  // TODO: uncomment the followings if needed.

  /*  - if your analysis requires explicitly typed pointers in IR. 
  cc_params[cc_par_cnt++] = (char*)"-Xclang";
  cc_params[cc_par_cnt++] = (char*)"-no-opaque-pointers"; */

  /*  - if your analysis precludes any inlining.
  cc_params[cc_par_cnt++] = (char*)"-Xclang";
  cc_params[cc_par_cnt++] = (char*)"-fno-inline"; */

  /*  - if your analysis needs some debugging information by default.
  cc_params[cc_par_cnt++] = (char*)"-g"; */

  while (--argc) {
    char* cur = *(++argv);
    cc_params[cc_par_cnt++] = cur;
  }
  cc_params[cc_par_cnt] = NULL;

  execvp(cc_params[0], (char**)cc_params);

  return 0;
}
