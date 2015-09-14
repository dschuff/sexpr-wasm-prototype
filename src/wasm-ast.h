#ifndef WASM_AST
#define WASM_AST

#include "wasm.h"

#include <string>
#include <vector>

namespace WasmAst {

// TODO(dschuff): Not sure whether pass-through the WasmFoo objects and just
// add extra things as needed, or duplicate everything to get a pure C++-y IR.
// For now just pass-through WasmType, WasmMemType, WasmOpType, WasmOpcode,
// WasmTokenType, WasmSourceLocation, WasmSourceRange, and WasmToken (which can
// hopefully be used as-is but create new structures for Variable, Function, and
// Module for easier use.

//const char* type_names[WASM_NUM_TYPES];

class Variable {
 public:
  WasmVariable* parse_variable;
  WasmType type;
  std::string local_name;  // Empty if none bound
};

class Function {
 public:
  WasmFunction* parse_function;
  WasmType result_type;
  std::vector<Variable> args;
  std::vector<Variable> locals;
  std::string local_name; // Empty if none bound
  std::string export_name; // Empty if not exported.
  // int export_index; // Is this needed?
  bool is_external;
  int depth;

  void dump() {
    printf("  (func ");
    if (local_name.size())
      printf("%s", local_name.c_str());
    for (auto &arg : args) {
      printf(" (param");
      if (arg.local_name.size())
        printf(" %s", arg.local_name.c_str());
      printf(" %d)", arg.type);
    }

    if (result_type != WASM_TYPE_VOID)
      printf(" (result %d)", result_type);

    printf(")\n");
  }
};


class Module {
 public:
  WasmModule* parse_module;
  std::vector<Function> functions;
  std::vector<Function*> exports;
  std::vector<WasmSegment> segments;
  uint32_t initial_memory_size;
  uint32_t max_memory_size;

  void dump() {
    printf("(module\n");
    for (auto& func : functions)
      func.dump();
    printf(")\n");
  }
};

}
#endif // WASM_AST
