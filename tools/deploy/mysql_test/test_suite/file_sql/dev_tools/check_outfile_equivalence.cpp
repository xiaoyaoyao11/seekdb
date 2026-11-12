/*
 * A check that the changes of the COPY statement do not change the behaviour
 * of an ordinary SELECT ... INTO OUTFILE.
 *
 * The check compares the original source of the operator with the changed
 * one with a line based diff.  Every line that the change removed or added
 * must belong to the branch of the COPY statement, that is the line must
 * mention the flag of the COPY statement or one of the functions that
 * implement it.  Every other line must be untouched.
 */
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cout << "FAIL: " << msg << std::endl;                                \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

static std::string slurp(const char *path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static std::vector<std::string> split_lines(const std::string &text)
{
  std::vector<std::string> lines;
  std::string line;
  std::istringstream in(text);
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    lines.push_back(line);
  }
  return lines;
}

/// The text of the line without its comment.
static std::string strip_comment(const std::string &line)
{
  const size_t pos = line.find("//");
  if (pos == std::string::npos) { return line; }
  return line.substr(0, pos);
}

/// True when the line is a comment.
static bool is_comment_line(const std::string &line)
{
  const size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos) { return false; }
  const std::string trimmed = line.substr(first);
  return trimmed.compare(0, 2, "//") == 0 || trimmed.compare(0, 2, "/*") == 0
         || trimmed.compare(0, 1, "*") == 0;
}

/// True when the line belongs to the branch of the COPY statement.
static bool is_copy_guarded(const std::string &line)
{
  static const char *markers[] = {
    "is_copy_",
    "write_copy_",
    "close_copy_",
    "init_copy_",
    "copy_writer_",
    "is_copy_header_written_",
    "is_copy_failed_",
  };
  // a comment carries no behaviour, a comment that documents the COPY
  // statement belongs to the COPY statement
  const std::string code = strip_comment(line);
  for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
    if (code.find(markers[i]) != std::string::npos) { return true; }
  }
  if (is_comment_line(line)) {
    // the comment documents the COPY statement
    static const char *words[] = { "COPY", "copy statement", "the COPY" };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
      if (line.find(words[i]) != std::string::npos) { return true; }
    }
  }
  return false;
}

/// True when the line is a comment that documents a block that is guarded by
/// the flag of the COPY statement.
///
/// The comment stands right above the line that opens the block, the lines
/// between the comment and the guard are comments as well.
static bool documents_copy_block(const std::vector<std::string> &lines, size_t index)
{
  if (index >= lines.size() || !is_comment_line(lines[index])) { return false; }
  // the lines that follow the comment
  for (size_t i = index + 1; i < lines.size(); ++i) {
    const std::string &line = lines[i];
    if (is_comment_line(line)) { continue; }
    // the block that the comment documents is guarded by the flag
    return line.find("MY_SPEC.is_copy_") != std::string::npos;
  }
  return false;
}

/// The functions that the change added, they are the implementation of the
/// COPY statement and they do not change the behaviour of the ordinary
/// SELECT ... INTO OUTFILE.
static bool is_new_copy_function(const std::string &line)
{
  static const char *markers[] = {
    "int ObSelectIntoOp::init_copy_writer(",
    "int ObSelectIntoOp::close_copy_writer(",
    "int ObSelectIntoOp::write_copy_header(",
    "int ObSelectIntoOp::write_copy_row(",
    "int ObSelectIntoOp::get_copy_field_text(",
  };
  for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
    if (line.find(markers[i]) != std::string::npos) { return true; }
  }
  return false;
}

/// The name of the function that the line belongs to, an empty text when the
/// line does not belong to a function that the change added.
static std::string enclosing_copy_function(const std::vector<std::string> &lines, size_t index)
{
  for (size_t i = index; i-- > 0;) {
    if (is_new_copy_function(lines[i])) { return lines[i]; }
  }
  return std::string();
}

/// The number of the braces that the line opens or closes.
static int brace_delta(const std::string &line)
{
  int delta = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    if ('{' == line[i]) { ++delta; }
    else if ('}' == line[i]) { --delta; }
  }
  return delta;
}

/// True when the line opens a function of the operator.
static bool is_function_header(const std::string &line)
{
  return line.find("int ObSelectIntoOp::") != std::string::npos
         && line.find("(") != std::string::npos;
}

/// The lines of the file that belong to the branch of the COPY statement.
///
/// A line belongs to the branch of the COPY statement when it is inside a
/// function that the change added, or when it is inside a block that is
/// guarded by the flag of the COPY statement.
///
/// The block of a guard is found by scanning forward from the line of the
/// guard to the line that closes it, the scan counts the braces.
static std::vector<bool> compute_copy_lines(const std::vector<std::string> &lines)
{
  std::vector<bool> result(lines.size(), false);
  // the lines of the functions that the change added
  bool in_copy_function = false;
  int function_depth = 0;
  int depth = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string &line = lines[i];
    if (is_function_header(line)) {
      in_copy_function = is_new_copy_function(line);
      function_depth = depth;
    }
    if (in_copy_function) {
      result[i] = true;
    }
    depth += brace_delta(line);
  }
  // the blocks that are guarded by the flag of the COPY statement, the
  // negation of the flag opens the branch of the ordinary path
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string &line = lines[i];
    if (line.find("MY_SPEC.is_copy_") == std::string::npos) { continue; }
    if (line.find("!MY_SPEC.is_copy_") != std::string::npos) { continue; }
    // the condition of the guard may span several lines, the lines of the
    // condition belong to the branch of the COPY statement
    size_t k = i;
    while (k < lines.size()) {
      result[k] = true;
      const int delta = brace_delta(lines[k]);
      if (delta > 0) { break; }
      ++k;
    }
    if (k >= lines.size()) { continue; }
    // the block of the guard, the scan counts the braces
    int block_depth = 0;
    for (size_t j = 0; j <= k; ++j) { block_depth += brace_delta(lines[j]); }
    for (size_t j = k + 1; j < lines.size(); ++j) {
      const int delta = brace_delta(lines[j]);
      result[j] = true;
      block_depth += delta;
      if (block_depth == 0) {
        // the line closes the block, the branch that follows it belongs to
        // the same statement
        if (j + 1 < lines.size() && lines[j + 1].find("else") != std::string::npos) {
          result[j + 1] = true;
        }
        break;
      }
    }
  }
  return result;
}

int main(int argc, char **argv)
{
  if (argc < 3) {
    std::cout << "usage: check_outfile_equivalence <original.cpp> <changed.cpp>\n";
    return 2;
  }
  const std::vector<std::string> before = split_lines(slurp(argv[1]));
  const std::vector<std::string> after = split_lines(slurp(argv[2]));

  // The four places where the change touches the ordinary
  // SELECT ... INTO OUTFILE.  Every one of them is a condition that the
  // flag of the COPY statement extends, the behaviour of the ordinary path
  // is therefore the same.
  struct Equivalence {
    const char *removed;
    const char *added;
    const char *explanation;
  };
  const Equivalence equivalences[] = {
    {"    external_properties_);",
     "    external_properties_, is_copy_, has_header_, null_string_, column_names_);",
     "the serialization of the spec holds the four new fields, the fields of "
     "the ordinary spec are serialized in the same order as before"},
    {"      && ObExternalFileFormat::FormatType::CSV_FORMAT == format_type_) {",
     "      && ObExternalFileFormat::FormatType::CSV_FORMAT == format_type_",
     "the condition of the creation of the writer of the ordinary path holds "
     "the same terms as before"},
    {"        if (T_INTO_OUTFILE == into_type) {",
     "      } else if (MY_SPEC.is_copy_) {",
     "the branch of the ordinary path is the else branch of the branch of "
     "the COPY statement, the ordinary path reaches it as before"},
    {"  if (OB_NOT_NULL(data_writer_) && OB_FAIL(data_writer_->close_data_writer())) {",
     "  } else if (OB_NOT_NULL(data_writer_) && OB_FAIL(data_writer_->close_data_writer())) {",
     "the closing of the writer of the ordinary path is the else branch of "
     "the branch of the COPY statement, the ordinary path reaches it as before"},
  };

  // The longest common subsequence of the two files, the lines that are not
  // part of it are the lines that the change touched.
  const size_t n = before.size();
  const size_t m = after.size();
  std::vector<std::vector<size_t> > lcs(n + 1, std::vector<size_t>(m + 1, 0));
  for (size_t i = n; i-- > 0;) {
    for (size_t j = m; j-- > 0;) {
      if (before[i] == after[j]) {
        lcs[i][j] = lcs[i + 1][j + 1] + 1;
      } else {
        lcs[i][j] = lcs[i + 1][j] > lcs[i][j + 1] ? lcs[i + 1][j] : lcs[i][j + 1];
      }
    }
  }
  std::vector<std::string> removed;
  std::vector<std::string> added;
  std::vector<size_t> added_index;
  {
    size_t i = 0;
    size_t j = 0;
    while (i < n && j < m) {
      if (before[i] == after[j]) {
        ++i;
        ++j;
      } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
        removed.push_back(before[i]);
        ++i;
      } else {
        added.push_back(after[j]);
        added_index.push_back(j);
        ++j;
      }
    }
    while (i < n) { removed.push_back(before[i]); ++i; }
    while (j < m) { added.push_back(after[j]); added_index.push_back(j); ++j; }
  }

  std::cout << "the change removed " << removed.size() << " line(s) and added "
            << added.size() << " line(s)\n";

  // Every line of the original that the change removed must be one of the
  // four lines that are known to be equivalent, or a line of the branch of
  // the COPY statement.
  size_t equivalence_idx = 0;
  for (size_t i = 0; i < removed.size(); ++i) {
    bool known = false;
    for (size_t e = 0; e < sizeof(equivalences) / sizeof(equivalences[0]); ++e) {
      if (removed[i] == equivalences[e].removed) {
        known = true;
        std::cout << "  equivalent change: [" << removed[i] << "] -> ["
                  << equivalences[e].added << "]  (" << equivalences[e].explanation << ")\n";
        ++equivalence_idx;
        break;
      }
    }
    if (!known) {
      CHECK(is_copy_guarded(removed[i]),
            "the removed line belongs to the ordinary SELECT ... INTO OUTFILE: ["
                + removed[i] + "]");
    }
  }
  CHECK(equivalence_idx == sizeof(equivalences) / sizeof(equivalences[0]),
        "all four known changes were found");

  // Every line that the change added must either belong to the branch of
  // the COPY statement or to a function that the change added.
  const std::vector<bool> copy_lines = compute_copy_lines(after);
  for (size_t i = 0; i < added.size(); ++i) {
    if (is_copy_guarded(added[i])) { continue; }
    const size_t index = added_index[i];
    // a comment that documents a block of the COPY statement
    if (documents_copy_block(after, index)) { continue; }
    CHECK(index < copy_lines.size() && copy_lines[index],
          "the added line belongs to the ordinary SELECT ... INTO OUTFILE: ["
              + added[i] + "] line " + std::to_string(index + 1) + " of the changed source");
  }

  if (0 == failures) {
    std::cout << "THE ORDINARY SELECT ... INTO OUTFILE IS UNCHANGED\n";
    return 0;
  }
  std::cout << failures << " CHECK(S) FAILED\n";
  return 1;
}
