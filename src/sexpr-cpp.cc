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


void dump_function(const WasmFunction& func, const WasmBinding* binding) {
  printf("  (func ");
  if (binding && binding->name)
    printf("%s", binding->name);
  if (func.num_args) {
    for (int i = 0; i < func.num_args; ++i) {
      printf(" (param");

      if (func.local_bindings.data[i].name)
        printf(" %s", func.local_bindings.data[i].name);
      printf(" %d)", func.locals.data[i].type);
    }
  }
  if (func.result_types.size) {
    printf(" (result");
    for (size_t i = 0; i < func.result_types.size; ++i) {
      printf(" %d", func.result_types.data[i]);
    }
    printf(")");
  }
  printf(")\n");
}


void dump_module(const WasmModule& mod) {
  printf("(module\n");
  for(size_t i = 0; i < mod.functions.size; ++i) {
    dump_function(mod.functions.data[i], &mod.function_bindings.data[i]);
  }
  printf(")\n");
}

class DumpParser : public wasm::Parser {
 public:
  DumpParser(const char *start, const char* end) : Parser(start, end) {
    printf("DumpParser %p\n", this);

  }
 protected:
  void AfterModule(WasmModule* m) override {
    //dump_module(*m);
  }
};


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

  DumpParser DumbParser(data, data + fsize);
  DumbParser.Parse();
  DumbParser.module.dump();

  //dump_module(mod);


  return 0;
}
