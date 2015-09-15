#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm.h"
#include "wasm-internal.h"
#include "wasm-parse.h"

#include "hash.h"

#define TABS_TO_SPACES 8
#define INITIAL_VECTOR_CAPACITY 8

#define FATAL_AT(loc, ...)                           \
  fprintf(stderr, "%d:%d: ", (loc).line, (loc).col), \
      fprintf(stderr, __VA_ARGS__), exit(1)

typedef struct OpInfo OpInfo;
typedef uint8_t MemAccess;

static void* append_element(void** data,
                            size_t* size,
                            size_t* capacity,
                            size_t elt_size);

#define DEFINE_VECTOR(name, type)                                           \
  void wasm_destroy_##name##_vector(type##Vector* vec) { free(vec->data); } \
  type* wasm_append_##name(type##Vector* vec) {                             \
    return append_element((void**)&vec->data, &vec->size, &vec->capacity,   \
                          sizeof(type));                                    \
  }

DEFINE_VECTOR(type, WasmType)
DEFINE_VECTOR(binding, WasmBinding)
DEFINE_VECTOR(variable, WasmVariable)
DEFINE_VECTOR(function, WasmFunction)
DEFINE_VECTOR(export, WasmExport)
DEFINE_VECTOR(import, WasmImport)
DEFINE_VECTOR(segment, WasmSegment)

typedef struct NameTypePair {
  const char* name;
  WasmType type;
} NameTypePair;

static NameTypePair s_types[] = {
    {"i32", WASM_TYPE_I32},
    {"i64", WASM_TYPE_I64},
    {"f32", WASM_TYPE_F32},
    {"f64", WASM_TYPE_F64},
};

static const char* s_type_names[] = {
    "void", "i32", "i64", "f32", "f64",
};
STATIC_ASSERT(ARRAY_SIZE(s_type_names) == WASM_NUM_TYPES);

static void* append_element(void** data,
                            size_t* size,
                            size_t* capacity,
                            size_t elt_size) {
  if (*size + 1 > *capacity) {
    size_t new_capacity = *capacity ? *capacity * 2 : INITIAL_VECTOR_CAPACITY;
    size_t new_size = new_capacity * elt_size;
    *data = realloc(*data, new_size);
    if (*data == NULL)
      FATAL("unable to alloc %zd bytes", new_size);
    *capacity = new_capacity;
  }
  return *data + (*size)++ * elt_size;
}

static void pop_binding(WasmBindingVector* vec) {
  assert(vec->size > 0);
  free(vec->data[vec->size - 1].name);
  vec->size--;
}

static int get_binding_by_name(WasmBindingVector* bindings, const char* name) {
  int i;
  for (i = 0; i < bindings->size; ++i) {
    WasmBinding* binding = &bindings->data[i];
    if (binding->name && strcmp(name, binding->name) == 0)
      return i;
  }
  return -1;
}

static const OpInfo* get_op_info(WasmToken t) {
  return in_word_set(t.range.start.pos,
                     (int)(t.range.end.pos - t.range.start.pos));
}

static int read_uint64(const char** s, const char* end, uint64_t* out) {
  if (*s == end)
    return 0;
  uint64_t value = 0;
  if (**s == '0' && *s + 1 < end && *(*s + 1) == 'x') {
    *s += 2;
    if (*s == end)
      return 0;
    for (; *s < end; ++(*s)) {
      uint32_t digit;
      char c = **s;
      if ((c - '0') <= 9) {
        digit = c - '0';
      } else if ((c - 'a') <= 6) {
        digit = 10 + (c - 'a');
      } else if ((c - 'A') <= 6) {
        digit = 10 + (c - 'A');
      } else {
        return 0;
      }
      uint64_t old_value = value;
      value = value * 16 + digit;
      /* check for overflow */
      if (old_value > value)
        return 0;
    }
  } else {
    for (; *s < end; ++(*s)) {
      uint32_t digit = (**s - '0');
      if (digit > 9)
        return 0;
      uint64_t old_value = value;
      value = value * 10 + digit;
      /* check for overflow */
      if (old_value > value)
        return 0;
    }
  }
  *out = value;
  return 1;
}

static int read_int64(const char** s, const char* end, uint64_t* out) {
  int has_sign = 0;
  if (**s == '-') {
    has_sign = 1;
    (*s)++;
  }
  uint64_t value;
  int result = read_uint64(s, end, &value);
  if (has_sign) {
    if (value > (uint64_t)INT64_MAX + 1) /* abs(INT64_MIN) == INT64_MAX + 1 */
      return 0;
    value = UINT64_MAX - value + 1;
  }
  *out = value;
  return result;
}

static int read_int32(const char** s,
                      const char* end,
                      uint32_t* out,
                      int allow_signed) {
  uint64_t value;
  int has_sign = 0;
  if (**s == '-') {
    if (!allow_signed)
      return 0;
    has_sign = 1;
    (*s)++;
  }
  int result = read_uint64(s, end, &value);
  if (has_sign) {
    if (value > (uint64_t)INT32_MAX + 1) /* abs(INT32_MIN) == INT32_MAX + 1 */
      return 0;
    value = UINT32_MAX - value + 1;
  } else {
    if (value > (uint64_t)UINT32_MAX)
      return 0;
  }
  *out = (uint32_t)value;
  return result;
}

static int read_double(const char** s, const char* end, double* out) {
  errno = 0;
  char* endptr;
  *out = strtod(*s, &endptr);
  if (endptr != end || errno != 0)
    return 0;
  *s = endptr;
  return 1;
}

static int read_uint32_token(WasmToken t, uint32_t* out) {
  const char* p = t.range.start.pos;
  return read_int32(&p, t.range.end.pos, out, 0);
}

static WasmToken read_token(WasmTokenizer* t) {
  WasmToken result;

  while (t->loc.pos < t->source.end) {
    switch (*t->loc.pos) {
      case ' ':
        t->loc.col++;
        t->loc.pos++;
        break;

      case '\t':
        t->loc.col += TABS_TO_SPACES;
        t->loc.pos++;
        break;

      case '\n':
        t->loc.line++;
        t->loc.col = 1;
        t->loc.pos++;
        break;

      case '"':
        result.type = WASM_TOKEN_TYPE_STRING;
        result.range.start = t->loc;
        t->loc.col++;
        t->loc.pos++;

        while (t->loc.pos < t->source.end) {
          switch (*t->loc.pos) {
            case '\\':
              if (t->loc.pos + 1 < t->source.end) {
                t->loc.col++;
                t->loc.pos++;
                if (*t->loc.pos == 'n' || *t->loc.pos == 't' ||
                    *t->loc.pos == '\\' || *t->loc.pos == '\'' ||
                    *t->loc.pos == '"') {
                  /* newline or tab */
                } else if (isxdigit(*t->loc.pos)) {
                  /* \xx for arbitrary value */
                  if (t->loc.pos + 1 < t->source.end) {
                    t->loc.col++;
                    t->loc.pos++;
                    if (!isxdigit(*t->loc.pos))
                      FATAL_AT(t->loc, "bad \\xx escape sequence\n");
                  } else {
                    FATAL_AT(t->loc, "eof in \\xx escape sequence\n");
                  }
                } else {
                  FATAL_AT(t->loc, "bad escape sequence\n");
                }
              } else {
                FATAL_AT(t->loc, "eof in escape sequence\n");
              }
              break;

            case '\n':
              FATAL_AT(t->loc, "newline in string\n");
              break;

            case '"':
              t->loc.col++;
              t->loc.pos++;
              goto done_string;
          }

          t->loc.col++;
          t->loc.pos++;
        }
      done_string:
        result.range.end = t->loc;
        return result;

      case ';':
        if (t->loc.pos + 1 < t->source.end && t->loc.pos[1] == ';') {
          /* line comment */
          while (t->loc.pos < t->source.end) {
            if (*t->loc.pos == '\n')
              break;

            t->loc.col++;
            t->loc.pos++;
          }
        }
        break;

      case '(':
        if (t->loc.pos + 1 < t->source.end && t->loc.pos[1] == ';') {
          int nesting = 1;
          t->loc.pos += 2;
          t->loc.col += 2;
          while (nesting > 0 && t->loc.pos < t->source.end) {
            switch (*t->loc.pos) {
              case ';':
                if (t->loc.pos + 1 < t->source.end && t->loc.pos[1] == ')') {
                  nesting--;
                  t->loc.col++;
                  t->loc.pos++;
                }
                t->loc.col++;
                t->loc.pos++;
                break;

              case '(':
                if (t->loc.pos + 1 < t->source.end && t->loc.pos[1] == ';') {
                  nesting++;
                  t->loc.col++;
                  t->loc.pos++;
                }
                t->loc.col++;
                t->loc.pos++;
                break;

              case '\t':
                t->loc.col += TABS_TO_SPACES;
                t->loc.pos++;
                break;

              case '\n':
                t->loc.line++;
                t->loc.col = 1;
                t->loc.pos++;
                break;

              default:
                t->loc.pos++;
                t->loc.col++;
                break;
            }
          }
          break;
        } else {
          result.type = WASM_TOKEN_TYPE_OPEN_PAREN;
          result.range.start = t->loc;
          t->loc.col++;
          t->loc.pos++;
          result.range.end = t->loc;
          return result;
        }
        break;

      case ')':
        result.type = WASM_TOKEN_TYPE_CLOSE_PAREN;
        result.range.start = t->loc;
        t->loc.col++;
        t->loc.pos++;
        result.range.end = t->loc;
        return result;

      default:
        result.type = WASM_TOKEN_TYPE_ATOM;
        result.range.start = t->loc;
        t->loc.col++;
        t->loc.pos++;
        while (t->loc.pos < t->source.end) {
          switch (*t->loc.pos) {
            case ' ':
            case '\t':
            case '\n':
            case '(':
            case ')':
              result.range.end = t->loc;
              return result;

            default:
              t->loc.col++;
              t->loc.pos++;
              break;
          }
        }
        break;
    }
  }

  result.type = WASM_TOKEN_TYPE_EOF;
  result.range.start = t->loc;
  result.range.end = t->loc;
  return result;
}

static void rewind_token(WasmTokenizer* tokenizer, WasmToken t) {
  tokenizer->loc = t.range.start;
}

static int hexdigit(char c) {
  if ((c - '0') <= 9) {
    return c - '0';
  } else if ((c - 'a') <= 6) {
    return 10 + (c - 'a');
  } else {
    assert((c - 'A') <= 6);
    return 10 + (c - 'A');
  }
}

static size_t string_contents_length(WasmToken t) {
  assert(t.type == WASM_TOKEN_TYPE_STRING);
  const char* src = t.range.start.pos + 1;
  const char* end = t.range.end.pos - 1;
  size_t length = 0;
  while (src < end) {
    if (*src == '\\') {
      src++;
      switch (*src) {
        case 'n':
        case 't':
        case '\\':
        case '\'':
        case '\"':
          src++;
          length++;
          break;

        default:
          /* must be a hex sequence */
          src += 2;
          length++;
          break;
      }
    } else {
      src++;
      length++;
    }
  }
  return length;
}

size_t wasm_copy_string_contents(WasmToken t, char* dest, size_t size) {
  assert(t.type == WASM_TOKEN_TYPE_STRING);
  const char* src = t.range.start.pos + 1;
  const char* end = t.range.end.pos - 1;

  char* dest_start = dest;

  while (src < end) {
    if (*src == '\\') {
      src++;
      switch (*src) {
        case 'n':
          *dest++ = '\n';
          break;
        case 't':
          *dest++ = '\t';
          break;
        case '\\':
          *dest++ = '\\';
          break;
        case '\'':
          *dest++ = '\'';
          break;
        case '\"':
          *dest++ = '\"';
          break;
        default:
          /* The string should be validated already, so we know this is a hex
           * sequence */
          *dest++ = (hexdigit(src[0]) << 4) | hexdigit(src[1]);
          src++;
          break;
      }
      src++;
    } else {
      *dest++ = *src++;
    }
  }
  /* return the data length */
  return dest - dest_start;
}

static char* dup_string_contents(WasmToken t) {
  assert(t.type == WASM_TOKEN_TYPE_STRING);
  const char* src = t.range.start.pos + 1;
  const char* end = t.range.end.pos - 1;
  /* Always allocate enough space for the entire string including the escape
   * characters. It will only get shorter, and this way we only have to iterate
   * through the string once. */
  size_t size = end - src + 1; /* +1 for \0 */
  char* result = malloc(size);
  size_t actual_size = wasm_copy_string_contents(t, result, size);
  result[actual_size] = '\0';
  return result;
}

static void expect(WasmToken t, WasmTokenType token_type, const char* desc) {
  if (t.type == WASM_TOKEN_TYPE_EOF) {
    FATAL_AT(t.range.start, "unexpected EOF\n");
  } else if (t.type != token_type) {
    FATAL_AT(t.range.start, "expected %s, not \"%.*s\"\n", desc,
             (int)(t.range.end.pos - t.range.start.pos), t.range.start.pos);
  }
}

static void unexpected_token(WasmToken t) {
  if (t.type == WASM_TOKEN_TYPE_EOF) {
    FATAL_AT(t.range.start, " unexpected EOF\n");
  } else {
    FATAL_AT(t.range.start, "unexpected token \"%.*s\"\n",
             (int)(t.range.end.pos - t.range.start.pos), t.range.start.pos);
  }
}

static void expect_open(WasmToken t) {
  expect(t, WASM_TOKEN_TYPE_OPEN_PAREN, "'('");
}

static void expect_close(WasmToken t) {
  expect(t, WASM_TOKEN_TYPE_CLOSE_PAREN, "')'");
}

static void expect_atom(WasmToken t) {
  expect(t, WASM_TOKEN_TYPE_ATOM, "ATOM");
}

static void expect_string(WasmToken t) {
  expect(t, WASM_TOKEN_TYPE_STRING, "STRING");
}

static void expect_var_name(WasmToken t) {
  expect_atom(t);
  if (t.range.end.pos - t.range.start.pos < 1 || t.range.start.pos[0] != '$')
    FATAL_AT(t.range.start, "expected name to begin with $\n");
}

static void expect_atom_op(WasmToken t, WasmOpType op, const char* desc) {
  expect_atom(t);
  const OpInfo* op_info = get_op_info(t);
  if (!op_info || op_info->op_type != op)
    FATAL_AT(t.range.start, "expected \"%s\"\n", desc);
}

static void check_opcode(WasmSourceLocation loc, WasmOpcode opcode) {
  if (opcode == WASM_OPCODE_INVALID)
    FATAL_AT(loc, "no opcode for instruction\n");
}

static int match_atom(WasmToken t, const char* s) {
  return strncmp(t.range.start.pos, s, t.range.end.pos - t.range.start.pos) ==
         0;
}

static int match_type(WasmToken t, WasmType* type) {
  int i;
  for (i = 0; i < ARRAY_SIZE(s_types); ++i) {
    if (match_atom(t, s_types[i].name)) {
      if (type)
        *type = s_types[i].type;
      return 1;
    }
  }
  return 0;
}

static int match_load_store_aligned(WasmToken t, OpInfo* op_info) {
  /* look for a slash near the end of the string */
  const char* p = t.range.start.pos;
  const char* end = t.range.end.pos - 1;
  while (end > p) {
    if (*end == '/') {
      const OpInfo* found = in_word_set(p, (int)(end - p));
      if (!(found && (found->op_type == WASM_OP_LOAD ||
                      found->op_type == WASM_OP_STORE)))
        return 0;

      ++end;
      uint32_t alignment;
      if (!read_int32(&end, t.range.end.pos, &alignment, 0))
        return 0;

      /* check that alignment is power-of-two */
      if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        FATAL_AT(t.range.start, "alignment must be power-of-two\n");

      int native_mem_type_alignment[] = {
          1, /* MEM_TYPE_I8 */
          2, /* MEM_TYPE_I16 */
          4, /* MEM_TYPE_I32 */
          8, /* MEM_TYPE_I64 */
          4, /* MEM_TYPE_F32 */
          8, /* MEM_TYPE_F64 */
      };

      *op_info = *found;
      if (alignment && alignment < native_mem_type_alignment[op_info->type2])
        op_info->access |= 0x8; /* unaligned access */
      return 1;
    }
    end--;
  }
  return 0;
}

static void check_type(WasmSourceLocation loc,
                       WasmType actual,
                       WasmType expected,
                       const char* desc) {
  if (actual != expected) {
    FATAL_AT(loc, "type mismatch%s. got %s, expected %s\n", desc,
             s_type_names[actual], s_type_names[expected]);
  }
}

static void check_type_arg(WasmSourceLocation loc,
                           WasmType actual,
                           WasmType expected,
                           const char* desc,
                           int arg_index) {
  if (actual != expected) {
    FATAL_AT(loc, "type mismatch for argument %d of %s. got %s, expected %s\n",
             arg_index, desc, s_type_names[actual], s_type_names[expected]);
  }
}

static WasmType get_result_type(WasmSourceLocation loc,
                                WasmFunction* function) {
  switch (function->result_types.size) {
    case 0:
      return WASM_TYPE_VOID;

    case 1:
      return function->result_types.data[0];

    default:
      FATAL_AT(loc, "multiple return values currently unsupported\n");
      return WASM_TYPE_VOID;
  }
}

static void parse_generic(WasmTokenizer* tokenizer) {
  int nesting = 1;
  while (nesting > 0) {
    WasmToken t = read_token(tokenizer);
    if (t.type == WASM_TOKEN_TYPE_OPEN_PAREN) {
      nesting++;
    } else if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN) {
      nesting--;
    } else if (t.type == WASM_TOKEN_TYPE_EOF) {
      FATAL_AT(t.range.start, "unexpected eof\n");
    }
  }
}

static int parse_var(WasmTokenizer* tokenizer,
                     WasmBindingVector* bindings,
                     int num_vars,
                     const char* desc) {
  WasmToken t = read_token(tokenizer);
  expect_atom(t);

  const char* p = t.range.start.pos;
  const char* end = t.range.end.pos;
  if (end - p >= 1 && p[0] == '$') {
    /* var name */
    int i;
    for (i = 0; i < bindings->size; ++i) {
      WasmBinding* binding = &bindings->data[i];
      const char* name = binding->name;
      /* The token is not NULL-terminated, so use strncmp. But make sure that
       * they're actually equal by checking for the NULL-terminator in name at
       * the same place */
      if (name && strncmp(name, p, end - p) == 0 && name[end - p] == 0)
        return binding->index;
    }
    FATAL_AT(t.range.start, "undefined %s variable \"%.*s\"\n", desc,
             (int)(end - p), p);
  } else {
    /* var index */
    uint32_t index;
    if (!read_int32(&p, t.range.end.pos, &index, 0) || p != t.range.end.pos) {
      FATAL_AT(t.range.start, "invalid var index\n");
    }

    if (index >= num_vars) {
      FATAL_AT(t.range.start, "%s variable out of range (max %d)\n", desc,
               num_vars);
    }

    return index;
  }
}

static int parse_function_var(WasmTokenizer* tokenizer, WasmModule* module) {
  return parse_var(tokenizer, &module->function_bindings,
                   module->functions.size, "function");
}

static int parse_import_var(WasmTokenizer* tokenizer, WasmModule* module) {
  return parse_var(tokenizer, &module->import_bindings, module->imports.size,
                   "import");
}

static int parse_global_var(WasmTokenizer* tokenizer, WasmModule* module) {
  return parse_var(tokenizer, &module->global_bindings, module->globals.size,
                   "global");
}

static int parse_local_var(WasmTokenizer* tokenizer, WasmFunction* function) {
  return parse_var(tokenizer, &function->local_bindings, function->locals.size,
                   "local");
}

/* Labels are indexed in reverse order (0 is the most recent, etc.), so handle
 * them separately */
static int parse_label_var(WasmTokenizer* tokenizer, WasmFunction* function) {
  WasmToken t = read_token(tokenizer);
  expect_atom(t);

  const char* p = t.range.start.pos;
  const char* end = t.range.end.pos;
  if (end - p >= 1 && p[0] == '$') {
    /* var name */
    int i;
    for (i = 0; i < function->labels.size; ++i) {
      WasmBinding* binding = &function->labels.data[i];
      const char* name = binding->name;
      if (name && strncmp(name, p, end - p) == 0)
        /* index is actually the current block depth */
        return binding->index;
    }
    FATAL_AT(t.range.start, "undefined label variable \"%.*s\"\n",
             (int)(end - p), p);
  } else {
    /* var index */
    uint32_t index;
    if (!read_int32(&p, t.range.end.pos, &index, 0) || p != t.range.end.pos) {
      FATAL_AT(t.range.start, "invalid var index\n");
    }

    if (index >= function->labels.size) {
      FATAL_AT(t.range.start, "label variable out of range (max %zd)\n",
               function->labels.size);
    }

    /* Index in reverse, so 0 is the most recent label. The stored "index" is
     * actually the block depth, so return that instead */
    index = (function->labels.size - 1) - index;
    return function->labels.data[index].index;
  }
}

static void parse_type(WasmTokenizer* tokenizer, WasmType* type) {
  WasmToken t = read_token(tokenizer);
  if (!match_type(t, type))
    FATAL_AT(t.range.start, "expected type\n");
}

static WasmType parse_expr(WasmParser* parser,
                           WasmTokenizer* tokenizer,
                           WasmModule* module,
                           WasmFunction* function);

static WasmType parse_block(WasmParser* parser,
                            WasmTokenizer* tokenizer,
                            WasmModule* module,
                            WasmFunction* function,
                            int* out_num_exprs) {
  int num_exprs = 0;
  WasmType type;
  while (1) {
    type = parse_expr(parser, tokenizer, module, function);
    ++num_exprs;
    WasmToken t = read_token(tokenizer);
    if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN)
      break;
    rewind_token(tokenizer, t);
  }
  *out_num_exprs = num_exprs;
  return type;
}

static void parse_literal(WasmParser* parser,
                          WasmTokenizer* tokenizer,
                          WasmType type) {
  WasmToken t = read_token(tokenizer);
  expect_atom(t);
  const char* p = t.range.start.pos;
  const char* end = t.range.end.pos;
  switch (type) {
    case WASM_TYPE_I32: {
      uint32_t value;
      if (!read_int32(&p, end, &value, 1))
        FATAL_AT(t.range.start, "invalid 32-bit int\n");
      parser->u32_literal(value, parser->user_data);
      break;
    }

    case WASM_TYPE_I64: {
      uint64_t value;
      if (!read_int64(&p, end, &value))
        FATAL_AT(t.range.start, "invalid 64-bit int\n");
      parser->u64_literal(value, parser->user_data);
      break;
    }

    case WASM_TYPE_F32:
    case WASM_TYPE_F64: {
      double value;
      if (!read_double(&p, end, &value))
        FATAL_AT(t.range.start, "invalid double\n");
      if (type == WASM_TYPE_F32)
        parser->f32_literal((float)value, parser->user_data);
      else
        parser->f64_literal(value, parser->user_data);
      break;
    }

    default:
      assert(0);
  }
}

static WasmType parse_switch(WasmParser* parser,
                             WasmTokenizer* tokenizer,
                             WasmModule* module,
                             WasmFunction* function,
                             WasmType in_type) {
  int num_cases = 0;
  WasmType cond_type = parse_expr(parser, tokenizer, module, function);
  check_type(tokenizer->loc, cond_type, in_type, " in switch condition");
  WasmType type = WASM_TYPE_VOID;
  WasmToken t = read_token(tokenizer);
  while (1) {
    expect_open(t);
    WasmToken open = t;
    t = read_token(tokenizer);
    expect_atom(t);
    if (match_atom(t, "case")) {
      parse_literal(parser, tokenizer, in_type);
      t = read_token(tokenizer);
      if (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
        rewind_token(tokenizer, t);
        WasmType value_type;
        while (1) {
          value_type = parse_expr(parser, tokenizer, module, function);
          t = read_token(tokenizer);
          if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN) {
            break;
          } else if (t.type == WASM_TOKEN_TYPE_ATOM &&
                     match_atom(t, "fallthrough")) {
            expect_close(read_token(tokenizer));
            break;
          }
          rewind_token(tokenizer, t);
        }

        if (++num_cases == 1) {
          type = value_type;
        } else if (value_type == WASM_TYPE_VOID) {
          type = WASM_TYPE_VOID;
        } else {
          check_type(t.range.start, value_type, type, " in switch case");
        }
      }
    } else {
      /* default case */
      rewind_token(tokenizer, open);
      WasmType value_type = parse_expr(parser, tokenizer, module, function);
      if (value_type == WASM_TYPE_VOID) {
        type = WASM_TYPE_VOID;
      } else {
        check_type(t.range.start, value_type, type, " in switch default case");
      }
      expect_close(read_token(tokenizer));
      break;
    }
    t = read_token(tokenizer);
    if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN)
      break;
  }
  return type;
}

static WasmType parse_expr(WasmParser* parser,
                           WasmTokenizer* tokenizer,
                           WasmModule* module,
                           WasmFunction* function) {
  WasmType type = WASM_TYPE_VOID;
  expect_open(read_token(tokenizer));
  WasmToken t = read_token(tokenizer);
  expect_atom(t);

  OpInfo op_info_aligned;
  const OpInfo* op_info = get_op_info(t);
  if (!op_info) {
    if (!match_load_store_aligned(t, &op_info_aligned))
      unexpected_token(t);
    op_info = &op_info_aligned;
  }
  switch (op_info->op_type) {
    case WASM_OP_BINARY: {
      check_opcode(t.range.start, op_info->opcode);
      parser->before_binary(op_info->opcode, parser->user_data);
      WasmType value0_type = parse_expr(parser, tokenizer, module, function);
      check_type_arg(t.range.start, value0_type, op_info->type, "binary op", 0);
      WasmType value1_type = parse_expr(parser, tokenizer, module, function);
      check_type_arg(t.range.start, value1_type, op_info->type, "binary op", 1);
      assert(value0_type == value1_type);
      type = value0_type;
      expect_close(read_token(tokenizer));
      break;
    }

    case WASM_OP_BLOCK: {
      WasmParserCookie cookie = parser->before_block(parser->user_data);
      function->depth++;
      int num_exprs;
      type = parse_block(parser, tokenizer, module, function, &num_exprs);
      function->depth--;
      parser->after_block(num_exprs, cookie, parser->user_data);
      break;
    }

    case WASM_OP_BREAK: {
      t = read_token(tokenizer);
      int depth = 0;
      if (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
        rewind_token(tokenizer, t);
        /* parse_label_var returns the depth counting up (so 0 is the topmost
         block of this function). We want the depth counting down (so 0 is the
         most recent block). */
        depth = (function->depth - 1) - parse_label_var(tokenizer, function);
        expect_close(read_token(tokenizer));
      } else if (function->labels.size == 0) {
        FATAL_AT(t.range.start, "label variable out of range (max 0)\n");
      }
      parser->after_break(depth, parser->user_data);
      break;
    }

    case WASM_OP_CALL: {
      int index = parse_function_var(tokenizer, module);
      parser->before_call(index, parser->user_data);
      WasmFunction* callee = &module->functions.data[index];

      int num_args = 0;
      while (1) {
        WasmToken t = read_token(tokenizer);
        if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN)
          break;
        rewind_token(tokenizer, t);
        if (++num_args > callee->num_args) {
          FATAL_AT(t.range.start,
                   "too many arguments to function. got %d, expected %d\n",
                   num_args, callee->num_args);
        }
        WasmType arg_type = parse_expr(parser, tokenizer, module, function);
        WasmType expected = callee->locals.data[num_args - 1].type;
        check_type_arg(t.range.start, arg_type, expected, "call", num_args - 1);
      }

      if (num_args < callee->num_args) {
        FATAL_AT(t.range.start,
                 "too few arguments to function. got %d, expected %d\n",
                 num_args, callee->num_args);
      }

      type = get_result_type(t.range.start, callee);
      break;
    }

    case WASM_OP_CALL_IMPORT: {
      int index = parse_import_var(tokenizer, module);
      parser->before_call_import(index, parser->user_data);
      WasmImport* callee = &module->imports.data[index];

      int num_args = 0;
      while (1) {
        WasmToken t = read_token(tokenizer);
        if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN)
          break;
        rewind_token(tokenizer, t);
        if (++num_args > callee->args.size) {
          FATAL_AT(t.range.start,
                   "too many arguments to function. got %d, expected %zd\n",
                   num_args, callee->args.size);
        }
        WasmType arg_type = parse_expr(parser, tokenizer, module, function);
        WasmType expected = callee->args.data[num_args - 1].type;
        check_type_arg(t.range.start, arg_type, expected, "call_import",
                       num_args - 1);
      }

      if (num_args < callee->args.size) {
        FATAL_AT(t.range.start,
                 "too few arguments to function. got %d, expected %zd\n",
                 num_args, callee->args.size);
      }

      type = callee->result_type;
      break;
    }

    case WASM_OP_CALL_INDIRECT:
      /* TODO(binji) */
      break;

    case WASM_OP_COMPARE: {
      check_opcode(t.range.start, op_info->opcode);
      parser->before_compare(op_info->opcode, parser->user_data);
      WasmType value0_type = parse_expr(parser, tokenizer, module, function);
      check_type_arg(t.range.start, value0_type, op_info->type, "compare op",
                     0);
      WasmType value1_type = parse_expr(parser, tokenizer, module, function);
      check_type_arg(t.range.start, value1_type, op_info->type, "compare op",
                     1);
      type = WASM_TYPE_I32;
      expect_close(read_token(tokenizer));
      break;
    }

    case WASM_OP_CONST:
      check_opcode(t.range.start, op_info->opcode);
      parser->before_const(op_info->opcode, parser->user_data);
      parse_literal(parser, tokenizer, op_info->type);
      expect_close(read_token(tokenizer));
      type = op_info->type;
      break;

    case WASM_OP_CONVERT: {
      check_opcode(t.range.start, op_info->opcode);
      parser->before_convert(op_info->opcode, parser->user_data);
      WasmType value_type = parse_expr(parser, tokenizer, module, function);
      check_type(t.range.start, value_type, op_info->type2, " of convert op");
      expect_close(read_token(tokenizer));
      type = op_info->type;
      break;
    }

    case WASM_OP_DESTRUCT:
      /* TODO(binji) */
      break;

    case WASM_OP_GET_LOCAL: {
      int index = parse_local_var(tokenizer, function);
      WasmVariable* variable = &function->locals.data[index];
      type = variable->type;
      expect_close(read_token(tokenizer));
      parser->after_get_local(variable->index, parser->user_data);
      break;
    }

    case WASM_OP_IF: {
      WasmParserCookie cookie = parser->before_if(parser->user_data);
      WasmType cond_type = parse_expr(parser, tokenizer, module, function);
      check_type(tokenizer->loc, cond_type, WASM_TYPE_I32, " of condition");
      WasmType true_type = parse_expr(parser, tokenizer, module, function);
      t = read_token(tokenizer);
      int with_else = 0;
      if (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
        with_else = 1;
        rewind_token(tokenizer, t);
        WasmType false_type = parse_expr(parser, tokenizer, module, function);
        if (true_type == WASM_TYPE_VOID || false_type == WASM_TYPE_VOID) {
          type = WASM_TYPE_VOID;
        } else {
          check_type(tokenizer->loc, false_type, true_type,
                     " between true and false branches");
          type = true_type;
        }
        expect_close(read_token(tokenizer));
      } else {
        type = true_type;
      }
      parser->after_if(with_else, cookie, parser->user_data);
      break;
    }

    case WASM_OP_LABEL: {
      WasmParserCookie cookie = parser->before_label(parser->user_data);

      t = read_token(tokenizer);
      WasmBinding* binding = wasm_append_binding(&function->labels);
      binding->name = NULL;
      binding->index = function->depth++;

      if (t.type == WASM_TOKEN_TYPE_ATOM) {
        /* label */
        expect_var_name(t);
        binding->name =
            strndup(t.range.start.pos, t.range.end.pos - t.range.start.pos);
      } else if (t.type == WASM_TOKEN_TYPE_OPEN_PAREN) {
        /* no label */
        rewind_token(tokenizer, t);
      } else {
        unexpected_token(t);
      }

      int num_exprs;
      type = parse_block(parser, tokenizer, module, function, &num_exprs);
      pop_binding(&function->labels);
      function->depth--;
      parser->after_label(num_exprs, cookie, parser->user_data);
      break;
    }

    case WASM_OP_LOAD: {
      check_opcode(t.range.start, op_info->opcode);
      parser->before_load(op_info->opcode, op_info->access, parser->user_data);
      WasmType index_type = parse_expr(parser, tokenizer, module, function);
      check_type(t.range.start, index_type, WASM_TYPE_I32, " of load index");
      expect_close(read_token(tokenizer));
      type = op_info->type;
      break;
    }

    case WASM_OP_LOAD_GLOBAL: {
      int index = parse_global_var(tokenizer, module);
      type = module->globals.data[index].type;
      expect_close(read_token(tokenizer));
      parser->after_load_global(index, parser->user_data);
      break;
    }

    case WASM_OP_LOOP: {
      WasmParserCookie cookie = parser->before_loop(parser->user_data);
      function->depth++;
      int num_exprs;
      type = parse_block(parser, tokenizer, module, function, &num_exprs);
      function->depth--;
      parser->after_loop(num_exprs, cookie, parser->user_data);
      break;
    }

    case WASM_OP_NOP:
      parser->after_nop(parser->user_data);
      expect_close(read_token(tokenizer));
      break;

    case WASM_OP_RETURN: {
      parser->before_return(parser->user_data);
      int num_results = 0;
      while (1) {
        t = read_token(tokenizer);
        if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN)
          break;
        if (++num_results > function->result_types.size) {
          FATAL_AT(t.range.start,
                   "too many return values. got %d, expected %zd\n",
                   num_results, function->result_types.size);
        }
        rewind_token(tokenizer, t);
        WasmType result_type = parse_expr(parser, tokenizer, module, function);
        WasmType expected = function->result_types.data[num_results - 1];
        check_type_arg(t.range.start, result_type, expected, "return",
                       num_results - 1);
      }

      if (num_results < function->result_types.size) {
        FATAL_AT(t.range.start, "too few return values. got %d, expected %zd\n",
                 num_results, function->result_types.size);
      }

      type = get_result_type(t.range.start, function);
      break;
    }

    case WASM_OP_SET_LOCAL: {
      int index = parse_local_var(tokenizer, function);
      WasmVariable* variable = &function->locals.data[index];
      parser->before_set_local(variable->index, parser->user_data);
      type = parse_expr(parser, tokenizer, module, function);
      check_type(t.range.start, type, variable->type, "");
      expect_close(read_token(tokenizer));
      break;
    }

    case WASM_OP_STORE: {
      check_opcode(t.range.start, op_info->opcode);
      parser->before_store(op_info->opcode, op_info->access, parser->user_data);
      WasmType index_type = parse_expr(parser, tokenizer, module, function);
      check_type(t.range.start, index_type, WASM_TYPE_I32, " of store index");
      WasmType value_type = parse_expr(parser, tokenizer, module, function);
      check_type(t.range.start, value_type, op_info->type, " of store value");
      expect_close(read_token(tokenizer));
      type = op_info->type;
      break;
    }

    case WASM_OP_STORE_GLOBAL: {
      int index = parse_global_var(tokenizer, module);
      parser->before_store_global(index, parser->user_data);
      WasmVariable* variable = &module->globals.data[index];
      type = parse_expr(parser, tokenizer, module, function);
      check_type(t.range.start, type, variable->type, "");
      expect_close(read_token(tokenizer));
      break;
    }

    case WASM_OP_SWITCH:
      check_opcode(t.range.start, op_info->opcode);
      type = parse_switch(parser, tokenizer, module, function, op_info->type);
      break;

    case WASM_OP_UNARY: {
      check_opcode(t.range.start, op_info->opcode);
      parser->before_unary(op_info->opcode, parser->user_data);
      WasmType value_type = parse_expr(parser, tokenizer, module, function);
      check_type(t.range.start, value_type, op_info->type, " of unary op");
      type = value_type;
      expect_close(read_token(tokenizer));
      break;
    }

    default:
      assert(0);
      break;
  }
  return type;
}

static int parse_func(WasmParser* parser,
                      WasmTokenizer* tokenizer,
                      WasmModule* module,
                      WasmFunction* function) {
  WasmToken t = read_token(tokenizer);
  if (t.type == WASM_TOKEN_TYPE_ATOM) {
    /* named function */
    t = read_token(tokenizer);
  }

  int num_exprs = 0;
  WasmType type = WASM_TYPE_VOID;
  while (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
    expect_open(t);
    WasmToken open = t;
    t = read_token(tokenizer);
    expect_atom(t);

    const OpInfo* op_info = get_op_info(t);
    switch (op_info ? op_info->op_type : WASM_OP_NONE) {
      case WASM_OP_PARAM:
      case WASM_OP_RESULT:
      case WASM_OP_LOCAL:
        /* Skip, this has already been pre-parsed */
        parse_generic(tokenizer);
        break;

      default:
        rewind_token(tokenizer, open);
        type = parse_expr(parser, tokenizer, module, function);
        ++num_exprs;
        break;
    }
    t = read_token(tokenizer);
  }

  WasmType result_type = get_result_type(t.range.start, function);
  if (result_type != WASM_TYPE_VOID)
    check_type(t.range.start, type, result_type, " in function result");

  return num_exprs;
}

static void parse_type_list(WasmTokenizer* tokenizer,
                            WasmVariableVector* variables, int allow_empty) {
  WasmToken t = read_token(tokenizer);
  WasmType type;
  if (allow_empty && t.type == WASM_TOKEN_TYPE_CLOSE_PAREN)
    return;
  if (!match_type(t, &type))
    FATAL_AT(t.range.start, "expected a type, not \"%.*s\"\n",
             (int)(t.range.end.pos - t.range.start.pos), t.range.start.pos);
  while (1) {
    WasmVariable* variable = wasm_append_variable(variables);
    variable->type = type;
    variable->offset = 0;
    variable->index = 0;

    t = read_token(tokenizer);
    if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN)
      break;
    else if (!match_type(t, &type))
      unexpected_token(t);
  }
}

static void preparse_binding_list(WasmTokenizer* tokenizer,
                                  WasmVariableVector* variables,
                                  WasmBindingVector* bindings,
                                  const char* desc) {
  WasmToken t = read_token(tokenizer);
  WasmType type;
  if (match_type(t, &type)) {
    rewind_token(tokenizer, t);
    parse_type_list(tokenizer, variables, 0);
  } else {
    expect_var_name(t);
    parse_type(tokenizer, &type);
    expect_close(read_token(tokenizer));

    char* name =
        strndup(t.range.start.pos, t.range.end.pos - t.range.start.pos);
    if (get_binding_by_name(bindings, name) != -1)
      FATAL_AT(t.range.start, "redefinition of %s \"%s\"\n", desc, name);

    WasmVariable* variable = wasm_append_variable(variables);
    variable->type = type;
    variable->offset = 0;
    variable->index = 0;

    WasmBinding* binding = wasm_append_binding(bindings);
    binding->name = name;
    binding->index = variables->size - 1;
  }
}

static void remap_locals(WasmFunction* function) {
  int max[WASM_NUM_TYPES] = {};
  int i;
  for (i = function->num_args; i < function->locals.size; ++i) {
    WasmVariable* variable = &function->locals.data[i];
    max[variable->type]++;
  }

  /* Args don't need remapping */
  for (i = 0; i < function->num_args; ++i)
    function->locals.data[i].index = i;

  int start[WASM_NUM_TYPES];
  start[WASM_TYPE_I32] = function->num_args;
  start[WASM_TYPE_I64] = start[WASM_TYPE_I32] + max[WASM_TYPE_I32];
  start[WASM_TYPE_F32] = start[WASM_TYPE_I64] + max[WASM_TYPE_I64];
  start[WASM_TYPE_F64] = start[WASM_TYPE_F32] + max[WASM_TYPE_F32];

  int seen[WASM_NUM_TYPES] = {};
  for (i = function->num_args; i < function->locals.size; ++i) {
    WasmVariable* variable = &function->locals.data[i];
    variable->index = start[variable->type] + seen[variable->type]++;
  }
}

static void preparse_func(WasmTokenizer* tokenizer, WasmModule* module) {
  WasmFunction* function = wasm_append_function(&module->functions);
  memset(function, 0, sizeof(*function));

  WasmToken t = read_token(tokenizer);
  if (t.type == WASM_TOKEN_TYPE_ATOM) {
    /* named function */
    char* name =
        strndup(t.range.start.pos, t.range.end.pos - t.range.start.pos);
    if (get_binding_by_name(&module->function_bindings, name) != -1)
      FATAL_AT(t.range.start, "redefinition of function \"%s\"\n", name);

    WasmBinding* binding = wasm_append_binding(&module->function_bindings);
    binding->name = name;
    binding->index = module->functions.size - 1;
    t = read_token(tokenizer);
  }

  while (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
    expect_open(t);
    t = read_token(tokenizer);
    expect_atom(t);

    const OpInfo* op_info = get_op_info(t);
    switch (op_info ? op_info->op_type : WASM_OP_NONE) {
      case WASM_OP_PARAM:
        /* Don't allow adding params after locals */
        if (function->num_args != function->locals.size)
          FATAL_AT(t.range.start, "parameters must come before locals\n");
        preparse_binding_list(tokenizer, &function->locals,
                              &function->local_bindings, "function argument");
        function->num_args = function->locals.size;
        break;

      case WASM_OP_RESULT: {
        t = read_token(tokenizer);
        while (1) {
          WasmType type;
          if (!match_type(t, &type))
            unexpected_token(t);
          *(WasmType*)wasm_append_type(&function->result_types) = type;

          t = read_token(tokenizer);
          if (t.type == WASM_TOKEN_TYPE_CLOSE_PAREN)
            break;
        }
        break;
      }

      case WASM_OP_LOCAL:
        preparse_binding_list(tokenizer, &function->locals,
                              &function->local_bindings, "local");
        break;

      default:
        rewind_token(tokenizer, t);
        parse_generic(tokenizer);
        break;
    }
    t = read_token(tokenizer);
  }

  remap_locals(function);
}

static void preparse_module(WasmTokenizer* tokenizer, WasmModule* module) {
  int seen_memory = 0;
  WasmToken t = read_token(tokenizer);
  WasmToken first = t;
  while (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
    expect_open(t);
    t = read_token(tokenizer);
    expect_atom(t);

    const OpInfo* op_info = get_op_info(t);
    switch (op_info ? op_info->op_type : WASM_OP_NONE) {
      case WASM_OP_FUNC:
        preparse_func(tokenizer, module);
        break;

      case WASM_OP_GLOBAL:
        preparse_binding_list(tokenizer, &module->globals,
                              &module->global_bindings, "global");
        break;

      case WASM_OP_MEMORY: {
        if (seen_memory)
          FATAL_AT(t.range.start, "only one memory block allowed\n");
        seen_memory = 1;
        t = read_token(tokenizer);
        expect_atom(t);
        if (!read_uint32_token(t, &module->initial_memory_size))
          FATAL_AT(t.range.start, "invalid initial memory size\n");

        t = read_token(tokenizer);
        if (t.type == WASM_TOKEN_TYPE_ATOM) {
          if (!read_uint32_token(t, &module->max_memory_size))
            FATAL_AT(t.range.start, "invalid max memory size\n");

          if (module->max_memory_size < module->initial_memory_size) {
            FATAL_AT(t.range.start,
                     "max size (%u) must be greater than or equal to initial "
                     "size (%u)\n",
                     module->max_memory_size, module->initial_memory_size);
          }

          t = read_token(tokenizer);
        } else {
          module->max_memory_size = module->initial_memory_size;
        }

        uint32_t last_segment_end = 0;
        while (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
          expect_open(t);
          t = read_token(tokenizer);
          if (!match_atom(t, "segment"))
            unexpected_token(t);
          t = read_token(tokenizer);
          expect_atom(t);

          WasmSegment* segment = wasm_append_segment(&module->segments);

          if (!read_uint32_token(t, &segment->address))
            FATAL_AT(t.range.start, "invalid memory segment address\n");

          if (segment->address < last_segment_end) {
            FATAL_AT(t.range.start,
                     "address (%u) less than end of previous segment (%u)\n",
                     segment->address, last_segment_end);
          }

          if (segment->address >= module->initial_memory_size) {
            FATAL_AT(t.range.start,
                     "address (%u) greater than initial memory size (%u)\n",
                     segment->address, module->initial_memory_size);
          }

          t = read_token(tokenizer);
          expect_string(t);
          segment->data = t;
          segment->size = string_contents_length(t);

          last_segment_end = segment->address + segment->size;

          if (last_segment_end > module->initial_memory_size) {
            FATAL_AT(
                t.range.start,
                "segment ends past the end of the initial memory size (%u)\n",
                module->initial_memory_size);
          }

          expect_close(read_token(tokenizer));
          t = read_token(tokenizer);
        }
        break;
      }

      case WASM_OP_IMPORT: {
        WasmImport* import = wasm_append_import(&module->imports);
        memset(import, 0, sizeof(*import));

        WasmToken t = read_token(tokenizer);
        if (t.type == WASM_TOKEN_TYPE_ATOM) {
          /* named import */
          expect_var_name(t);
          char* name =
              strndup(t.range.start.pos, t.range.end.pos - t.range.start.pos);
          if (get_binding_by_name(&module->import_bindings, name) != -1)
            FATAL_AT(t.range.start, "redefinition of import \"%s\"\n", name);

          WasmBinding* binding = wasm_append_binding(&module->import_bindings);
          binding->name = name;
          binding->index = module->imports.size - 1;
          t = read_token(tokenizer);
        }

        expect_string(t);
        import->module_name = dup_string_contents(t);
        t = read_token(tokenizer);
        expect_string(t);
        import->func_name = dup_string_contents(t);
        t = read_token(tokenizer);
        /* ( param is required */
        expect_open(t);
        t = read_token(tokenizer);
        expect_atom_op(t, WASM_OP_PARAM, "param");
        parse_type_list(tokenizer, &import->args, 1);
        t = read_token(tokenizer);
        if (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
          expect_open(t);
          t = read_token(tokenizer);
          expect_atom_op(t, WASM_OP_RESULT, "result");
          parse_type(tokenizer, &import->result_type);
          expect_close(read_token(tokenizer)); /* close result */
          expect_close(read_token(tokenizer)); /* close import */
        } else {
          import->result_type = WASM_TYPE_VOID;
        }
        break;
      }

      default:
        parse_generic(tokenizer);
        break;
    }
    t = read_token(tokenizer);
  }
  rewind_token(tokenizer, first);
}

static void wasm_destroy_binding_list(WasmBindingVector* bindings) {
  int i;
  for (i = 0; i < bindings->size; ++i)
    free(bindings->data[i].name);
  wasm_destroy_binding_vector(bindings);
}

static void wasm_destroy_module(WasmModule* module) {
  int i;
  for (i = 0; i < module->functions.size; ++i) {
    WasmFunction* function = &module->functions.data[i];
    wasm_destroy_type_vector(&function->result_types);
    wasm_destroy_variable_vector(&function->locals);
    wasm_destroy_binding_list(&function->local_bindings);
    wasm_destroy_binding_list(&function->labels);
  }
  wasm_destroy_binding_list(&module->function_bindings);
  wasm_destroy_binding_list(&module->global_bindings);
  wasm_destroy_function_vector(&module->functions);
  wasm_destroy_variable_vector(&module->globals);
  for (i = 0; i < module->exports.size; ++i)
    free(module->exports.data[i].name);
  wasm_destroy_export_vector(&module->exports);
  wasm_destroy_binding_list(&module->import_bindings);
  for (i = 0; i < module->imports.size; ++i) {
    WasmImport* import = &module->imports.data[i];
    free(import->module_name);
    free(import->func_name);
    wasm_destroy_variable_vector(&import->args);
  }
  wasm_destroy_import_vector(&module->imports);
  wasm_destroy_segment_vector(&module->segments);
}

static void parse_module(WasmParser* parser, WasmTokenizer* tokenizer) {
  WasmModule module = {};
  preparse_module(tokenizer, &module);
  parser->before_module(&module, parser->user_data);

  int function_index = 0;
  WasmToken t = read_token(tokenizer);
  while (t.type != WASM_TOKEN_TYPE_CLOSE_PAREN) {
    expect_open(t);
    t = read_token(tokenizer);
    expect_atom(t);

    const OpInfo* op_info = get_op_info(t);
    switch (op_info ? op_info->op_type : WASM_OP_NONE) {
      case WASM_OP_FUNC: {
        WasmFunction* function = &module.functions.data[function_index++];
        parser->before_function(&module, function, parser->user_data);
        int num_exprs = parse_func(parser, tokenizer, &module, function);
        parser->after_function(&module, function, num_exprs, parser->user_data);
        break;
      }

      case WASM_OP_EXPORT: {
        parser->before_export(&module, parser->user_data);
        t = read_token(tokenizer);
        expect_string(t);
        int index = parse_function_var(tokenizer, &module);

        WasmExport* export = wasm_append_export(&module.exports);
        export->name = dup_string_contents(t);
        export->index = index;

        expect_close(read_token(tokenizer));
        parser->after_export(&module, export, parser->user_data);
        break;
      }

      case WASM_OP_TABLE:
        parse_generic(tokenizer);
        break;

      case WASM_OP_GLOBAL:
      case WASM_OP_MEMORY:
      case WASM_OP_IMPORT:
        /* Skip, this has already been pre-parsed */
        parse_generic(tokenizer);
        break;

      default:
        unexpected_token(t);
        break;
    }
    t = read_token(tokenizer);
  }

  parser->after_module(&module, parser->user_data);

  wasm_destroy_module(&module);
}

void wasm_parse_file(WasmParser* parser, WasmTokenizer* tokenizer) {
  WasmToken t = read_token(tokenizer);
  while (t.type != WASM_TOKEN_TYPE_EOF) {
    expect_open(t);
    t = read_token(tokenizer);
    expect_atom(t);

    const OpInfo* op_info = get_op_info(t);
    switch (op_info ? op_info->op_type : WASM_OP_NONE) {
      case WASM_OP_MODULE:
        parse_module(parser, tokenizer);
        break;

      case WASM_OP_ASSERT_EQ:
      case WASM_OP_ASSERT_INVALID:
      case WASM_OP_INVOKE:
        parse_generic(tokenizer);
        break;

      default:
        unexpected_token(t);
        break;
    }
    t = read_token(tokenizer);
  }
}
