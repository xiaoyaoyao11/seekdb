// Compile the actual COPY header; syscall wrapping only schedules real attacks
// or injects I/O failures. No writer implementation is copied into this test.
#include "sql/engine/basic/file_sql_copy.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <grp.h>
using namespace oceanbase::sql;
using namespace oceanbase::common;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x " failed errno=" << errno << '\n'; std::exit(1); } } while (0)
static std::string root;
static std::string read(const std::string &p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}}; }
static void put(const std::string &p, const std::string &s) { std::ofstream f(p); f << s; CHECK(f.good()); }
static int start(FileSqlCopyWriter &w, bool header = true) {
  FileSqlPlan p; p.path = root + "/out.csv"; p.options.format = "csv"; p.options.header = header;
  p.schema.columns = {{"id", FileSqlType::BIGINT, false}, {"text", FileSqlType::VARCHAR, true}};
  std::string d = p.encode(); return w.open(ObString(static_cast<int32_t>(d.size()), d.data()), root);
}
static void collision() {
  const std::string prefix = root + "/.seekdb-copy-" + std::to_string(getpid()) + "-";
  for (int i = 1; i <= 64; ++i) put(prefix + std::to_string(i), "sentinel-" + std::to_string(i));
  { FileSqlCopyWriter w; (void)start(w); }
  for (int i = 1; i <= 64; ++i) CHECK(read(prefix + std::to_string(i)) == "sentinel-" + std::to_string(i));
  CHECK(!fs::exists(root + "/out.csv"));
  CHECK(std::distance(fs::directory_iterator(root), fs::directory_iterator{}) == 64);
}
static void success() {
  FileSqlCopyWriter w; CHECK(start(w) == OB_SUCCESS);
  CHECK(!fs::exists(root + "/out.csv"));
  CHECK(w.write({{{false, "1"}, {false, "a,\"b"}}}) == OB_SUCCESS);
  CHECK(w.write({{{false, "2"}, {true, ""}}}) == OB_SUCCESS);
  CHECK(w.write({{{false, "3"}, {false, ""}}}) == OB_SUCCESS);
  CHECK(w.write({{{false, "4"}, {false, "NULL"}}}) == OB_SUCCESS);
  CHECK(w.finish() == OB_SUCCESS); w.close(); w.close();
  CHECK(read(root + "/out.csv") == "id,text\n1,\"a,\"\"b\"\n2,NULL\n3,\"\"\n4,\"NULL\"\n");
  CHECK(std::distance(fs::directory_iterator(root), fs::directory_iterator{}) == 1);
  CHECK(w.finish() == OB_NOT_INIT);
}
static void cancelled() {
  { FileSqlCopyWriter w; CHECK(start(w) == OB_SUCCESS); CHECK(w.write({{{false,"x"},{false,"y"}}}) == OB_SUCCESS); }
  CHECK(fs::is_empty(root));
  { FileSqlCopyWriter w; CHECK(start(w) == OB_SUCCESS); CHECK(w.write({{{false,"bad-width"}}}) == OB_INVALID_ARGUMENT); }
  CHECK(fs::is_empty(root));
}
// Wrappers execute the real syscall. Replacement is scheduled at fsync or
// immediately before linkat, so a stat-then-link workaround also fails RED.
extern "C" int __real_fsync(int);
extern "C" int __real_linkat(int, const char *, int, const char *, int);
static bool replace_on_sync = false, replace_on_link = false, use_symlink = false;
static int attacks = 0;
static void replace_source(int dir, const char *name) {
  CHECK(unlinkat(dir, name, 0) == 0);
  if (use_symlink) CHECK(symlinkat("victim", dir, name) == 0);
  else { int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL, 0600); CHECK(fd >= 0); CHECK(::write(fd, "unrelated", 9) == 9); CHECK(::close(fd) == 0); }
  ++attacks;
}
extern "C" int __wrap_fsync(int fd) {
  int result = __real_fsync(fd);
  if (replace_on_sync) {
    replace_on_sync = false;
    for (const auto &e : fs::directory_iterator(root)) {
      const auto name = e.path().filename().string();
      if (name.find(".seekdb-copy-") == 0 && e.is_regular_file()) replace_source(AT_FDCWD, e.path().c_str());
    }
    // Anonymous staging has no name to replace. Supply a hostile decoy
    // instead and verify the writer neither publishes nor cleans it up.
    if (!attacks) { put(root + "/.seekdb-copy-decoy", "decoy"); replace_source(AT_FDCWD, (root + "/.seekdb-copy-decoy").c_str()); }
  }
  return result;
}
extern "C" int __wrap_linkat(int olddir, const char *oldname, int newdir, const char *newname, int flags) {
  if (replace_on_link) {
    replace_on_link = false;
    if (std::string(oldname).find(".seekdb-copy-") != std::string::npos) replace_source(olddir, oldname);
    else { put(root + "/.seekdb-copy-decoy", "decoy"); replace_source(AT_FDCWD, (root + "/.seekdb-copy-decoy").c_str()); }
  }
  return __real_linkat(olddir, oldname, newdir, newname, flags);
}
static void replacement(bool symlink, bool at_link) {
  put(root + "/victim", "unrelated");
  { FileSqlCopyWriter w; CHECK(start(w) == OB_SUCCESS);
    use_symlink = symlink; replace_on_link = at_link; replace_on_sync = !at_link;
    int ret = w.finish(); CHECK(attacks == 1);
    if (ret == OB_SUCCESS) {
      CHECK(!fs::is_symlink(root + "/out.csv")); CHECK(read(root + "/out.csv") == "id,text\n");
    } else CHECK(!fs::exists(fs::symlink_status(root + "/out.csv")));
  }
  CHECK(read(root + "/victim") == "unrelated");
  CHECK(fs::exists(root + "/.seekdb-copy-decoy"));
}
static void regular_sync() { replacement(false, false); }
static void symlink_sync() { replacement(true, false); }
static void regular_link() { replacement(false, true); }
static void symlink_link() { replacement(true, true); }
static void existing() {
  put(root + "/out.csv", "keep");
  { FileSqlCopyWriter w; CHECK(start(w) == OB_FILE_ALREADY_EXIST); }
  CHECK(read(root + "/out.csv") == "keep"); CHECK(std::distance(fs::directory_iterator(root), fs::directory_iterator{}) == 1);
  fs::remove(root + "/out.csv");
  { FileSqlCopyWriter w; CHECK(start(w) == OB_SUCCESS); put(root + "/out.csv", "racer"); CHECK(w.finish() == OB_FILE_ALREADY_EXIST); }
  CHECK(read(root + "/out.csv") == "racer"); CHECK(std::distance(fs::directory_iterator(root), fs::directory_iterator{}) == 1);
}
int main(int argc, char **argv) {
  CHECK(argc == 2); CHECK(chdir(argv[1]) == 0);
  // Runner strips every capability, including CAP_DAC_READ_SEARCH, before
  // execution. Keep uid 0 only to traverse the /root-owned scratch ancestors.
  CHECK(read("/proc/self/status").find("CapEff:\t0000000000000000") != std::string::npos);
  std::cout << "uid=" << getuid() << " euid=" << geteuid() << " CapEff=0" << std::endl;
  int failures = 0;
  for (auto test : {std::pair<const char *, void(*)()>{"collision64", collision}, {"success", success}, {"cancelled", cancelled}, {"existing_target", existing}, {"replace_regular_at_fsync", regular_sync}, {"replace_symlink_at_fsync", symlink_sync}, {"replace_regular_at_link", regular_link}, {"replace_symlink_at_link", symlink_link}}) {
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) {
      char name[] = "copy-native-XXXXXX"; CHECK(mkdtemp(name) != nullptr); CHECK(chdir(name) == 0); root = ".";
      test.second(); CHECK(chdir("..") == 0); fs::remove_all(name); std::cout << "PASS " << test.first << std::endl; std::exit(0);
    }
    int status; CHECK(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { ++failures; std::cout << "FAIL " << test.first << std::endl; }
  }
  return failures ? 1 : 0;
}
