#include "wasm-cpp.h"
#include <cstdio>
#include <cassert>

namespace wasm {
void Parser::Unimplemented(const char* name) {
  printf("%s\n", name);
}
void Parser::BeforeModule(WasmModule* m) {
  module.parse_module = m;
  module.max_memory_size = m->max_memory_size;
  module.initial_memory_size = m->initial_memory_size;

  module.functions.reserve(m->functions.size);
  for (size_t i = 0; i < m->functions.size; ++i) {
    WasmFunction *parser_func = &m->functions.data[i];
    module.functions.emplace_back();
    auto &func = module.functions.back();

    func.index_in_module = i;
    assert(parser_func->result_types.size < 2);
    func.result_type = parser_func->result_types.size ?
        parser_func->result_types.data[0] : WASM_TYPE_VOID;

    func.args.reserve(parser_func->num_args);
    for (int j = 0; j < parser_func->num_args; ++j) {
      func.args.emplace_back();
      func.args.back().type = parser_func->locals.data[j].type;
    }
    for (size_t j = parser_func->num_args; j < parser_func->locals.size; ++j) {
      func.locals.emplace_back();
      func.locals.back().type = parser_func->locals.data[j].type;
    }
    for (size_t j = 0; j < parser_func->local_bindings.size; ++j) {
      auto &binding = parser_func->local_bindings.data[j];
      if (binding.index < parser_func->num_args) {
        func.args[binding.index].local_name.assign(binding.name);
      } else {
        func.locals[binding.index - parser_func->num_args].local_name.assign(
            binding.name);
      }
    }
  }
  for (size_t i = 0; i < m->function_bindings.size; ++i) {
    auto &binding = m->function_bindings.data[i];
    module.functions[binding.index].local_name.assign(binding.name);
  }

  for (size_t i = 0; i < m->segments.size; ++i)
    module.segments.emplace_back(m->segments.data[i]);
}

void Parser::AfterModule(WasmModule* m) {
  Unimplemented("AfterModule");
}

void Parser::BeforeFunction(WasmModule* m, WasmFunction* f) {
  Unimplemented("BeforeFunction");
}

void Parser::AfterExport(WasmModule* m, WasmExport *e) {
  WasmAst::Function *f = &module.functions[e->index];
  f->export_name =  e->name;
  module.exports.push_back(f);
}

}
