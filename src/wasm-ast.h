#ifndef WASM_AST
#define WASM_AST

#include "wasm.h"

#include <string>
#include <vector>

namespace WasmAst {

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
  WasmType type = WASM_TYPE_VOID;
  std::string local_name;  // Empty if none bound
};

class Function {
 public:
  WasmType result_type = WASM_TYPE_VOID;
  std::vector<Variable> args;
  std::vector<Variable> locals;
  std::string local_name; // Empty if none bound
  std::string export_name; // Empty if not exported.
  int index_in_module = 0;
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
      printf(" (result %s)", TypeName(result_type));
    printf(")\n");
  }
};


class Segment {
 public:
  size_t size = 0;
  size_t address = 0;
  std::vector<char> initial_data;
  std::string as_string() const { return std::string(initial_data.begin(),
                                                     initial_data.end()); }
};

class Module {
 public:
  std::vector<Function> functions;
  std::vector<Function*> exports;
  std::vector<Segment> segments;
  uint32_t initial_memory_size = 0;
  uint32_t max_memory_size = 0;

  void dump() {
    printf("(module\n");
    for (auto& func : functions)
      func.dump();

    if (initial_memory_size) {
      printf("(memory %u", initial_memory_size);
      if (max_memory_size)
        printf(" %u", max_memory_size);
      for (auto& seg : segments) {
        printf("(segment %u \"%s\")\n", seg.address, seg.as_string().c_str());
      }
      printf(")\n");
    }

    for (auto* ex : exports) {
      printf("(export \"%s\" %u)", ex->export_name.c_str(), ex->index_in_module);
    }
    printf(")\n");
  }
};

}
#endif // WASM_AST
