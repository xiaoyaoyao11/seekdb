// Standalone Linux regressions linked against the complete real utils TU.
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>
#include <sys/wait.h>
#include <unistd.h>
using namespace oceanbase::common;
class MallocAllocator final : public ObIAllocator {
public:
  ~MallocAllocator() override
  {
    for (void *block : blocks_) { std::free(block); }
  }
  void fail_allocations() { fail_allocations_ = true; }
  void *alloc(int64_t size) override
  {
    if (fail_allocations_) { return nullptr; }
    void *block = size < 0 ? nullptr : std::malloc(size > 0 ? size : 1);
    if (block != nullptr) { blocks_.push_back(block); }
    return block;
  }
  void *alloc(int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *block) override
  {
    for (void *&owned : blocks_) {
      if (owned == block) {
        std::free(owned);
        owned = nullptr;
        break;
      }
    }
  }
private:
  bool fail_allocations_ = false;
  std::vector<void *> blocks_;
};
namespace {
void setup(int rc)
{
  if (rc != OB_SUCCESS) { throw rc; }
}
std::string bytes(const ObString &s)
{
  return std::string(s.ptr() != nullptr ? s.ptr() : "", s.length());
}

struct Fixture {
  MallocAllocator allocator;
  ObJsonString *string(const std::string &value)
  {
    ObJsonString *node = nullptr;
    ObString source(static_cast<int32_t>(value.size()), value.data());
    ObString input;
    // ObJsonString borrows its bytes: copy temporary inputs into our arena.
    setup(ob_write_string(allocator, source, input));
    setup(ObAIFuncJsonUtils::get_json_string(allocator, input, node));
    return node;
  }
  ObJsonArray *array()
  {
    ObJsonArray *node = nullptr;
    setup(ObAIFuncJsonUtils::get_json_array(allocator, node));
    return node;
  }
  ObJsonObject *object()
  {
    ObJsonObject *node = nullptr;
    setup(ObAIFuncJsonUtils::get_json_object(allocator, node));
    return node;
  }
  ObJsonInt *integer()
  {
    ObJsonInt *node = nullptr;
    setup(ObAIFuncJsonUtils::get_json_int(allocator, 7, node));
    return node;
  }
  ObJsonArray *strings(const std::vector<std::string> &values)
  {
    ObJsonArray *node = array();
    for (const std::string &value : values) { setup(node->append(string(value))); }
    return node;
  }
  ObJsonObject *prompt(const std::string &pattern, ObJsonArray *args)
  {
    ObString source(static_cast<int32_t>(pattern.size()), pattern.data());
    ObString input;
    setup(ob_write_string(allocator, source, input));
    ObJsonObject *node = nullptr;
    setup(ObAIFuncPromptObjectUtils::construct_prompt_object(allocator, input, args, node));
    return node;
  }
  std::string serialize(ObJsonNode *node)
  {
    ObString output;
    setup(ObAIFuncJsonUtils::print_json_to_str(allocator, node, output));
    return bytes(output);
  }
  bool render(ObJsonObject *node, const std::string &expected)
  {
    ObString output;
    const int rc = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, node, output);
    const bool pass = rc == OB_SUCCESS && bytes(output) == expected;
    if (!pass) {
      std::printf("  rc=%d actual_length=%d expected_length=%zu\n", rc, output.length(), expected.size());
    }
    return pass;
  }
  bool rejects(ObJsonObject *node)
  {
    ObString output;
    const int rc = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, node, output);
    if (rc != OB_INVALID_ARGUMENT) { std::printf("  unexpected rejection rc=%d\n", rc); }
    return rc == OB_INVALID_ARGUMENT;
  }
};
struct Test {
  const char *name;
  std::function<bool(Fixture &)> check;
};
} // namespace

int main()
{
  std::vector<Test> tests;
  auto add = [&](const char *name, std::function<bool(Fixture &)> check) {
    tests.push_back({name, check});
  };
  auto text = [&](const char *name, const std::string &pattern,
                  const std::vector<std::string> &args, const std::string &expected) {
    add(name, [=](Fixture &f) { return f.render(f.prompt(pattern, f.strings(args)), expected); });
  };
  const std::string long_value(4096, 'x');
  add("repeated_long_placeholder", [=](Fixture &f) {
    ObJsonObject *node = f.prompt("{0}/{0}/{0}", f.strings({long_value}));
    return ObAIFuncPromptObjectUtils::is_valid_prompt_object(node)
        && f.render(node, long_value + "/" + long_value + "/" + long_value);
  });
  // Fresh owned nodes for every tree; never reparent a previous argument.
  auto array_prompt = [=](Fixture &f) {
    ObJsonArray *args = f.array();
    setup(args->append(f.strings({long_value})));
    return f.prompt("{0}", args);
  };
  add("json_array_argument_validator", [=](Fixture &f) {
    return ObAIFuncPromptObjectUtils::is_valid_prompt_object(array_prompt(f));
  });
  add("json_array_argument_rendering", [=](Fixture &f) {
    return f.render(array_prompt(f), "[\"" + long_value + "\"]");
  });
  add("json_object_argument", [](Fixture &f) {
    ObJsonObject *value = f.object();
    setup(value->add(ObString("key"), f.string("value")));
    const std::string expected = f.serialize(value);
    ObJsonArray *args = f.array();
    setup(args->append(value));
    ObJsonObject *node = f.prompt("before:{0}:after", args);
    return ObAIFuncPromptObjectUtils::is_valid_prompt_object(node)
        && f.render(node, "before:" + expected + ":after");
  });
  add("nested_prompt_serialized_not_expanded", [](Fixture &f) {
    ObJsonObject *nested = f.prompt("inner:{0}", f.strings({"payload"}));
    const std::string expected = f.serialize(nested);
    ObJsonArray *args = f.array();
    setup(args->append(nested));
    return f.render(f.prompt("outer:{0}", args), "outer:" + expected);
  });
  add("repeated_structured_arguments", [](Fixture &f) {
    ObJsonObject *value = f.object();
    setup(value->add(ObString("k"), f.string(std::string(4096, 'v'))));
    ObJsonArray *array = f.strings({"array", "{0}"});
    const std::string first = f.serialize(value);
    const std::string second = f.serialize(array);
    ObJsonArray *args = f.array();
    setup(args->append(value));
    setup(args->append(array));
    return f.render(f.prompt("{0}|{1}|{0}|{1}", args),
                    first + "|" + second + "|" + first + "|" + second);
  });
  std::vector<std::string> many;
  for (int i = 0; i < 12; ++i) { many.push_back("arg" + std::to_string(i)); }
  text("multidigit_indices", "{10}/{11}/{1}", many, "arg10/arg11/arg1");
  text("leading_zero_indices", "{00010}/{01}/{000}", many, "arg10/arg1/arg0");
  text("many_leading_zeros", "{0000000000000000000000000000000000000000}", {"zero"}, "zero");
  const std::string malformed = "{}|{x}|{-1}|{ 0}|{0x}|{0|}|}|{";
  text("malformed_braces_literal", malformed, {}, malformed);
  text("surrounding_braces", "{{0}}{0}", {"x"}, "{x}x");
  text("empty_argument", "a{0}b", {""}, "ab");
  text("empty_rendered_output", "{0}{0}", {""}, "");
  text("empty_args_literal_template", "no placeholders", {}, "no placeholders");
  const std::string literal = std::string("中文🙂\"\\\n") + std::string("\0tail", 5);
  text("literal_bytes_in_template", literal + "{0}" + literal, {"X"}, literal + "X" + literal);
  text("literal_bytes_in_argument", "<{0}>", {literal}, "<" + literal + ">");
  text("argument_placeholder_not_recursive", "{0}/{1}", {"{1}{999}", "done"}, "{1}{999}/done");
  text("unreferenced_argument", "literal", {"unused"}, "literal");
  auto reject_index = [&](const char *name, const std::string &pattern) {
    add(name, [=](Fixture &f) { return f.rejects(f.prompt(pattern, f.strings({"x"}))); });
  };
  reject_index("out_of_range_index", "{1}");
  reject_index("int64_max_index", "{9223372036854775807}");
  reject_index("int64_overflow_index", "{9223372036854775808}");
  reject_index("uint64_wraparound_index", "{18446744073709551616}");
  reject_index("huge_overflow_index", "{" + std::string(200, '9') + "}");
  add("placeholder_with_empty_args_rejected", [](Fixture &f) {
    return f.rejects(f.prompt("{0}", f.array()));
  });
  add("null_prompt_rejected", [](Fixture &f) {
    return !ObAIFuncPromptObjectUtils::is_valid_prompt_object(nullptr) && f.rejects(nullptr);
  });
  add("missing_envelope_keys_rejected", [](Fixture &f) {
    ObJsonObject *node = f.object();
    return !ObAIFuncPromptObjectUtils::is_valid_prompt_object(node) && f.rejects(node);
  });
  const char *invalid_names[] = {"array_template_rejected", "integer_template_rejected",
      "string_args_rejected", "object_args_rejected", "integer_argument_rejected"};
  for (int kind = 0; kind < 5; ++kind) {
    add(invalid_names[kind], [=](Fixture &f) {
      ObJsonObject *node = f.object();
      ObJsonNode *pattern = kind == 0 ? static_cast<ObJsonNode *>(f.array())
          : kind == 1 ? static_cast<ObJsonNode *>(f.integer()) : f.string("{0}");
      ObJsonNode *args = nullptr;
      if (kind == 2) { args = f.string("wrong"); }
      else if (kind == 3) { args = f.object(); }
      else {
        ObJsonArray *array = f.array();
        setup(array->append(kind == 4 ? static_cast<ObJsonNode *>(f.integer()) : f.string("x")));
        args = array;
      }
      setup(node->add(ObAIFuncPromptObjectUtils::prompt_template_key, pattern));
      setup(node->add(ObAIFuncPromptObjectUtils::prompt_args_key, args));
      const bool valid = ObAIFuncPromptObjectUtils::is_valid_prompt_object(node);
      const bool rejected = f.rejects(node);
      return !valid && rejected;
    });
  }
  add("empty_template_constructor_rejected", [](Fixture &f) {
    ObString empty;
    ObJsonObject *node = nullptr;
    return OB_INVALID_ARGUMENT == ObAIFuncPromptObjectUtils::construct_prompt_object(
        f.allocator, empty, f.array(), node);
  });

  add("output_allocation_failure_propagated", [](Fixture &f) {
    ObJsonObject *node = f.prompt("{0}", f.strings({"payload"}));
    f.allocator.fail_allocations();
    ObString output;
    return OB_ALLOCATE_MEMORY_FAILED ==
        ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(f.allocator, node, output);
  });
  add("rendered_length_overflow_rejected_before_allocation", [](Fixture &f) {
    const std::string value(1024 * 1024, 'x');
    std::string pattern;
    for (int i = 0; i < 2048; ++i) { pattern += "{0}"; }
    ObJsonObject *node = f.prompt(pattern, f.strings({value}));
    ObString output;
    return OB_SIZE_OVERFLOW ==
        ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(f.allocator, node, output);
  });
  size_t passed = 0;
  size_t failed = 0;
  size_t blocked = 0;
  // Isolate unsafe baseline casts so crashes cannot hide subsequent results.
  for (const Test &test : tests) {
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
      int result = 2;
      try {
        Fixture fixture;
        result = test.check(fixture) ? 0 : 1;
      } catch (int rc) {
        std::printf("  SETUP ERROR rc=%d\n", rc);
      } catch (...) {
        std::printf("  SETUP ERROR unexpected exception\n");
      }
      std::fflush(nullptr);
      _exit(result);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child) {
      ++failed;
      ++blocked;
      std::printf("%s: FAIL (process setup blocker)\n", test.name);
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      ++passed;
      std::printf("%s: PASS\n", test.name);
    } else {
      ++failed;
      if (WIFEXITED(status) && WEXITSTATUS(status) == 2) { ++blocked; }
      std::printf("%s: FAIL (exit=%d signal=%d)\n", test.name,
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                  WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
  }
  std::printf("TOTAL cases=%zu passed=%zu failed=%zu setup_blockers=%zu\n",
              tests.size(), passed, failed, blocked);
  return blocked != 0 ? 2 : failed != 0 ? 1 : 0;
}
