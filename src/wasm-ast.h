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

inline static const char* TypeName(WasmType t) {
  switch(t) {
    case WASM_TYPE_VOID: return "void";
    case WASM_TYPE_I32: return "i32";
    case WASM_TYPE_I64: return "i64";
    case WASM_TYPE_F32: return "f32";
    case WASM_TYPE_F64: return "f64";
    default: return "(unknown type)";
  }
}

class Variable {
 public:
  WasmVariable* parse_variable;
  WasmType type;
  std::string local_name;  // Empty if none bound
};

class Function {
 public:
  WasmFunction* parse_function = nullptr;
  WasmType result_type = WASM_TYPE_VOID;
  std::vector<Variable> args;
  std::vector<Variable> locals;
  std::string local_name; // Empty if none bound
  std::string export_name; // Empty if not exported.
  int index_in_module = 0;
  // int export_index; // Is this needed?
  bool is_external = false;
  int depth = 0;

  void dump() {
    printf("  (func ");
    if (local_name.size())
      printf("%s", local_name.c_str());
    for (auto &arg : args) {
      printf(" (param");
      if (arg.local_name.size())
        printf(" %s", arg.local_name.c_str());
      printf(" %s)", TypeName(arg.type));
    }

    if (result_type != WASM_TYPE_VOID)
      printf(" (result %d)", result_type);
    printf(")\n");
  }
};


inline static std::string
string_from_token(const WasmToken& t) {
  if (t.type != WASM_TOKEN_TYPE_STRING)
    return std::string();
  return std::string(t.range.start.pos,
                     t.range.end.pos);
}

class Module {
 public:
  WasmModule* parse_module = nullptr;
  std::vector<Function> functions;
  std::vector<Function*> exports;
  std::vector<WasmSegment> segments; // TODO(dschuff) dup the seg data?
  uint32_t initial_memory_size = 0;
  uint32_t max_memory_size = 0;

  void dump() {
    printf("(module\n");
    for (auto& func : functions)
      func.dump();

    if (initial_memory_size)
      printf("(memory %u\n", initial_memory_size);
    for (auto& seg : segments) {
      printf("(segment %u %s)\n", seg.address,
             string_from_token(seg.data).c_str());
    }
    if (initial_memory_size)
      printf(")\n");

    for (auto* ex : exports) {
      printf("(export \"%s\" %u)", ex->export_name.c_str(), ex->index_in_module);
    }
    printf(")\n");
  }
};

}
#endif // WASM_AST
