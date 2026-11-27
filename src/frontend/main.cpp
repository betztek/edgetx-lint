// edgetx-lint dispatcher.
//
// EdgeTX's storage layout is fixed at compile time per board and per
// release, so each (radio, EdgeTX tag) pair is a separate backend binary
// named edgetx-lint-<radio>-<tag>. This program only picks the right one and
// exec()s it with the same arguments.
//
// Backend search order:
//   $EDGETX_LINT_BACKEND_DIR
//   <dir of this exe>/../libexec/edgetx-lint
//   <dir of this exe>/libexec/edgetx-lint
//   <dir of this exe>

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* kPrefix = "edgetx-lint-";

static std::string exeDir()
{
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  std::string p(buf);
  return p.substr(0, p.find_last_of('/'));
}

static std::vector<std::string> searchDirs()
{
  std::vector<std::string> d;
  if (const char* e = getenv("EDGETX_LINT_BACKEND_DIR")) d.push_back(e);
  std::string x = exeDir();
  d.push_back(x + "/../libexec/edgetx-lint");
  d.push_back(x + "/libexec/edgetx-lint");
  d.push_back(x);
  return d;
}

struct Backend {
  std::string radio, tag, path;
};

static std::vector<int> verKey(const std::string& tag)
{
  std::vector<int> v;
  int a = 0, b = 0, c = 0;
  sscanf(tag.c_str(), "v%d.%d.%d", &a, &b, &c);
  return {a, b, c};
}

static std::vector<Backend> listBackends()
{
  std::vector<Backend> out;
  for (auto& dir : searchDirs()) {
    DIR* d = opendir(dir.c_str());
    if (!d) continue;
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.rfind(kPrefix, 0) != 0) continue;
      std::string rest = n.substr(strlen(kPrefix));  // <radio>-v<tag>
      auto dash = rest.find("-v");
      if (dash == std::string::npos) continue;
      std::string path = dir + "/" + n;
      if (access(path.c_str(), X_OK) != 0) continue;
      Backend b{rest.substr(0, dash), rest.substr(dash + 1), path};
      bool dup = std::any_of(out.begin(), out.end(), [&](const Backend& o) {
        return o.radio == b.radio && o.tag == b.tag;
      });
      if (!dup) out.push_back(b);
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end(), [](const Backend& a, const Backend& b) {
    return a.radio != b.radio ? a.radio < b.radio : verKey(a.tag) < verKey(b.tag);
  });
  return out;
}

static void usage(FILE* f)
{
  fprintf(f,
          "edgetx-lint %s - validate EdgeTX model/radio YAML with EdgeTX's own parser\n"
          "\n"
          "usage: edgetx-lint check --radio RADIO [--edgetx TAG] [options] FILE...\n"
          "       edgetx-lint describe --radio RADIO [--edgetx TAG] [--multi Multi.txt]\n"
          "                            [RADIO/radio.yml] MODEL...   facts JSON (what the radio loads)\n"
          "       edgetx-lint list            show installed radio/EdgeTX backends\n"
          "       edgetx-lint --version\n"
          "\n"
          "options:\n"
          "  --radio RADIO       tx16s | pocket\n"
          "  --edgetx TAG        EdgeTX release to lint against, e.g. v2.12.5\n"
          "                      (default: newest installed for RADIO)\n"
          "  --format FMT        text (default) | json | github\n"
          "  --warn-unknown      report unknown keys as warnings, not errors\n"
          "  --strict            exit 1 on warnings too\n"
          "  --quiet             hide info findings\n"
          "  --type model|radio  force file type (default: radio.yml => radio)\n"
          "  --no-roundtrip      skip the load/write/diff tier\n"
          "\n"
          "exit status: 0 clean, 1 findings (errors; warnings with --strict),\n"
          "             2 usage or internal error\n",
          LINT_VERSION);
}

int main(int argc, char** argv)
{
  if (argc < 2) {
    usage(stderr);
    return 2;
  }
  std::string cmd = argv[1];
  if (cmd == "--version" || cmd == "version") {
    printf("edgetx-lint %s\n", LINT_VERSION);
    return 0;
  }
  if (cmd == "-h" || cmd == "--help" || cmd == "help") {
    usage(stdout);
    return 0;
  }
  auto backends = listBackends();
  if (cmd == "list") {
    if (backends.empty()) printf("no backends installed\n");
    for (auto& b : backends) printf("%-8s %-10s %s\n", b.radio.c_str(), b.tag.c_str(), b.path.c_str());
    return 0;
  }
  if (cmd != "check" && cmd != "describe" && cmd != "dump-default" && cmd != "roundtrip") {
    fprintf(stderr, "edgetx-lint: unknown command '%s'\n\n", cmd.c_str());
    usage(stderr);
    return 2;
  }

  std::string radio, tag;
  for (int i = 2; i + 1 < argc; ++i) {
    if (!strcmp(argv[i], "--radio")) radio = argv[i + 1];
    if (!strcmp(argv[i], "--edgetx")) tag = argv[i + 1];
  }
  if (radio.empty()) {
    fprintf(stderr, "edgetx-lint: --radio is required (tx16s|pocket)\n");
    return 2;
  }
  if (!tag.empty() && tag[0] != 'v') tag = "v" + tag;

  const Backend* pick = nullptr;
  for (auto& b : backends) {
    if (b.radio != radio) continue;
    if (tag.empty() || b.tag == tag) pick = &b;  // sorted: last = newest
  }
  if (!pick) {
    fprintf(stderr, "edgetx-lint: no backend for radio '%s'%s%s. Installed:\n", radio.c_str(),
            tag.empty() ? "" : " and EdgeTX ", tag.c_str());
    for (auto& b : backends) fprintf(stderr, "  %s %s\n", b.radio.c_str(), b.tag.c_str());
    if (backends.empty()) fprintf(stderr, "  (none)\n");
    return 2;
  }

  std::vector<char*> args;
  args.push_back(const_cast<char*>(pick->path.c_str()));
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  args.push_back(nullptr);
  execv(pick->path.c_str(), args.data());
  perror("edgetx-lint: exec backend");
  return 2;
}
