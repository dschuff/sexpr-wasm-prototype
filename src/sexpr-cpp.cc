#include "wasm.h"
#include "wasm-parse.h"

#include "wasm-cpp.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <cstdlib>

static llvm::cl::opt<std::string>
InputFilename(llvm::cl::Positional, llvm::cl::desc("<input sexpr file>"),
              llvm::cl::init("-"));



int main(int argc, char** argv) {

  llvm::cl::ParseCommandLineOptions(argc, argv, "wasm->LLVM AOT\n");

  if (InputFilename == "-") {
    llvm::errs() << "Usage: " << argv[0] << ": <input filename>\n";
    return 1;
  }

  FILE *f = fopen(InputFilename.c_str(), "rb");
  if (!f) {
    llvm::errs() << "unable to read " << InputFilename << "\n";
    return 1;
  }

  fseek(f, 0, SEEK_END);
  size_t fsize = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* data = new char[fsize + 1];
  if (fread(data, 1, fsize, f) != fsize) {
    llvm::errs() <<"unable to read " << fsize << " bytes from " <<
        InputFilename << "\n";
    return 1;
  }
  fclose(f);

  data[fsize] = '\0';
  puts(data);

  wasm::Parser DumbParser(data, data + fsize);
  DumbParser.Parse();
  DumbParser.module.dump();

  return 0;
}
