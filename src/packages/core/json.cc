#include "base/package_api.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
// Count containers, not scalar leaves. Both directions enforce the same bound.
constexpr size_t kMaxDepth = 128;

// The SAX parser owns partially built LPC values outside the VM stack. Keep
// every allocation under RAII, including the current key/value, so malformed
// input and runtime allocation limits cannot leak on error()'s C++ unwind.
class JsonValue {
 public:
  svalue_t value = const0u;

  JsonValue() = default;
  JsonValue(const JsonValue&) = delete;
  JsonValue& operator=(const JsonValue&) = delete;
  JsonValue(JsonValue&& other) noexcept : value(other.value) { other.value = const0u; }
  ~JsonValue() { free_svalue(&value, "JSON temporary"); }

  void move_into(svalue_t* destination) {
    // Use the VM's out-of-line assignment for array item[1] tail slots too;
    // an inlined struct store trips object-size UBSan on that legacy layout.
    assign_svalue(destination, &value);
    free_svalue(&value, "JSON transferred value");
    value = const0u;
  }
};

void json_string(JsonValue& result, const std::string& text) {
  if (text.size() > static_cast<size_t>(CONFIG_INT(__MAX_STRING_LENGTH__))) {
    error("json_decode(): maximum string length exceeded.\n");
  }
  // LPC strings cannot represent embedded NULs. Refuse instead of truncating
  // either a value or a mapping key (which could alias another key).
  if (text.find('\0') != std::string::npos) {
    error("json_decode(): NUL is not representable in an LPC string.\n");
  }
  char* copy = new_string(text.size(), "json_decode");
  memcpy(copy, text.data(), text.size());
  copy[text.size()] = '\0';
  result.value.type = T_STRING;
  result.value.subtype = STRING_MALLOC;
  result.value.u.string = copy;
}

class JsonDecoder : public nlohmann::json_sax<json> {
  struct Frame {
    JsonValue object;
    JsonValue key;
    std::vector<JsonValue> items;
  };
  std::vector<std::unique_ptr<Frame>> frames;

  bool append(JsonValue item) {
    if (frames.empty()) {
      item.move_into(&result.value);
    } else {
      auto& frame = *frames.back();
      if (frame.object.value.type == T_MAPPING) {
        // find_for_insert also enforces maximum mapping size. Duplicate JSON
        // keys replace earlier values, matching ordinary mapping assignment.
        item.move_into(find_for_insert(frame.object.value.u.map, &frame.key.value, 0));
      } else {
        if (frame.items.size() >= static_cast<size_t>(CONFIG_INT(__MAX_ARRAY_SIZE__))) {
          error("json_decode(): maximum array size exceeded.\n");
        }
        frame.items.push_back(std::move(item));
      }
    }
    return true;
  }

  bool start(bool object) {
    if (frames.size() >= kMaxDepth) {
      error("json_decode(): maximum nesting depth of %zu exceeded.\n", kMaxDepth);
    }
    auto frame = std::make_unique<Frame>();
    if (object) {
      auto* mapping = allocate_mapping(0);
      frame->object.value.type = T_MAPPING;
      frame->object.value.subtype = 0;
      frame->object.value.u.map = mapping;
    }
    frames.push_back(std::move(frame));
    return true;
  }

 public:
  JsonValue result;

  bool null() override { return append(JsonValue()); }
  bool boolean(bool value) override { return number_integer(value ? 1 : 0); }
  bool number_integer(number_integer_t value) override {
    JsonValue item;
    item.value = const0;
    item.value.u.number = value;
    return append(std::move(item));
  }
  bool number_unsigned(number_unsigned_t value) override {
    if (value > static_cast<number_unsigned_t>(LPC_INT_MAX)) {
      error("json_decode(): integer is outside the LPC int range.\n");
    }
    return number_integer(static_cast<LPC_INT>(value));
  }
  bool number_float(number_float_t value, const string_t& token) override {
    // nlohmann falls back to double for integers outside uint64/int64. Do not
    // silently round such an integer into a different value or an LPC float.
    if (token.find_first_of(".eE") == std::string::npos) {
      error("json_decode(): integer is outside the LPC int range.\n");
    }
    if (!std::isfinite(value)) {
      error("json_decode(): number is outside the LPC float range.\n");
    }
    // A nonzero mantissa that underflows completely must not become a saved
    // zero. Subnormal values that remain representable are accepted.
    const auto exponent = token.find_first_of("eE");
    const auto nonzero = token.find_first_of("123456789");
    if (value == 0.0 && nonzero != std::string::npos && nonzero < exponent) {
      error("json_decode(): number is outside the LPC float range.\n");
    }
    JsonValue item;
    item.value.type = T_REAL;
    item.value.subtype = 0;
    item.value.u.real = value;
    return append(std::move(item));
  }
  bool string(string_t& value) override {
    JsonValue item;
    json_string(item, value);
    return append(std::move(item));
  }
  bool binary(binary_t&) override {
    error("json_decode(): binary values are not JSON.\n");
  }
  bool start_object(size_t) override { return start(true); }
  bool key(string_t& value) override {
    JsonValue key;
    json_string(key, value);
    key.move_into(&frames.back()->key.value);
    return true;
  }
  bool end_object() override {
    JsonValue value(std::move(frames.back()->object));
    frames.pop_back();
    return append(std::move(value));
  }
  bool start_array(size_t) override { return start(false); }
  bool end_array() override {
    auto& items = frames.back()->items;
    JsonValue value;
    auto* array = allocate_array(items.size());
    value.value.type = T_ARRAY;
    value.value.subtype = 0;
    value.value.u.arr = array;
    for (size_t i = 0; i < items.size(); ++i) {
      items[i].move_into(&array->item[i]);
    }
    frames.pop_back();
    return append(std::move(value));
  }
  bool parse_error(size_t position, const std::string&, const json::exception& exception) override {
    error("json_decode(): invalid JSON at byte %zu: %s\n", position, exception.what());
  }
};

class JsonEncoder {
  std::string output;
  std::unordered_set<const void*> active;
  const size_t limit = CONFIG_INT(__MAX_STRING_LENGTH__);

  void append(const std::string& text) {
    if (text.size() > limit - output.size()) {
      error("json_encode(): maximum string length exceeded.\n");
    }
    output += text;
  }

  void container(const void* identity, size_t depth) {
    if (depth >= kMaxDepth) {
      error("json_encode(): maximum nesting depth of %zu exceeded.\n", kMaxDepth);
    }
    if (!active.insert(identity).second) {
      error("json_encode(): circular reference.\n");
    }
  }

  void encode(const svalue_t* value, size_t depth) {
    switch (value->type) {
      case T_NUMBER:
        // Increment/decrement may retain a zero's undefined subtype. Match
        // undefinedp(): a nonzero integer is defined regardless of that tag.
        append(value->u.number == 0 && value->subtype == T_UNDEFINED
                   ? "null"
                   : std::to_string(value->u.number));
        return;
      case T_REAL:
        if (!std::isfinite(value->u.real)) {
          error("json_encode(): non-finite floats are not JSON numbers.\n");
        }
        // The vendored serializer emits enough digits to round-trip a double.
        append(json(value->u.real).dump());
        return;
      case T_STRING:
        // Also validates UTF-8 and escapes control characters, including ANSI.
        append(json(std::string(value->u.string, SVALUE_STRLEN(value))).dump());
        return;
      case T_ARRAY: {
        const auto* array = value->u.arr;
        container(array, depth);
        append("[");
        for (int i = 0; i < array->size; ++i) {
          if (i) append(",");
          encode(&array->item[i], depth + 1);
        }
        append("]");
        active.erase(array);
        return;
      }
      case T_MAPPING: {
        const auto* mapping = value->u.map;
        container(mapping, depth);
        std::vector<const mapping_node_t*> entries;
        entries.reserve(MAP_COUNT(mapping));
        for (unsigned int i = 0; i <= mapping->table_size; ++i) {
          for (auto* node = mapping->table[i]; node; node = node->next) {
            if (node->values[0].type != T_STRING) {
              error("json_encode(): mapping keys must be strings.\n");
            }
            entries.push_back(node);
          }
        }
        // Stable output does not depend on the mapping's hash bucket order.
        std::sort(entries.begin(), entries.end(), [](const auto* a, const auto* b) {
          return strcmp(a->values[0].u.string, b->values[0].u.string) < 0;
        });
        append("{");
        bool first = true;
        for (const auto* node : entries) {
          if (!first) append(",");
          first = false;
          encode(&node->values[0], depth + 1);
          append(":");
          encode(&node->values[1], depth + 1);
        }
        append("}");
        active.erase(mapping);
        return;
      }
      default:
        error("json_encode(): unsupported LPC type '%s'.\n", type_name(value->type));
    }
  }

 public:
  std::string run(const svalue_t* value) {
    encode(value, 0);
    return std::move(output);
  }
};
}  // namespace

void f_json_encode() {
  try {
    const auto text = JsonEncoder().run(sp);
    JsonValue result;
    json_string(result, text);
    result.move_into(sp);
  } catch (const json::exception& exception) {
    error("json_encode(): %s\n", exception.what());
  } catch (const std::bad_alloc&) {
    error("json_encode(): out of memory.\n");
  }
}

void f_json_decode() {
  try {
    const char* text;
    size_t size;
    if (sp->type == T_BUFFER) {
      text = reinterpret_cast<const char*>(sp->u.buf->item);
      size = sp->u.buf->size;
    } else {
      text = sp->u.string;
      size = SVALUE_STRLEN(sp);
    }
    // The parser treats a literal NUL as end-of-input even with an iterator
    // range. Do not let trailing garbage after NUL pass the strict parse.
    if (memchr(text, '\0', size)) {
      error("json_decode(): input contains a NUL byte.\n");
    }
    JsonDecoder decoder;
    if (!json::sax_parse(text, text + size, &decoder)) {
      error("json_decode(): invalid JSON.\n");
    }
    decoder.result.move_into(sp);
  } catch (const json::exception& exception) {
    error("json_decode(): %s\n", exception.what());
  } catch (const std::bad_alloc&) {
    error("json_decode(): out of memory.\n");
  }
}
