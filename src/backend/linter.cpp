// edgetx-lint: core engine
//
// Everything here drives EdgeTX's OWN code:
//  - YamlParser + YamlTreeWalker with the stock callback table, wrapped (not
//    replaced) to observe what the radio does with every key/value;
//  - readModelYaml()/loadRadioSettingsYaml() for the authoritative load;
//  - YamlTreeWalker::generate() for what the radio would write back.
// No EdgeTX source is modified.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "linter.h"
#include "schema.h"

#include "etx.h"

// --------------------------------------------------------------------------
// small utils

static std::string baseName(const std::string& p)
{
  auto s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

static std::string dirName(const std::string& p)
{
  auto s = p.find_last_of('/');
  if (s == std::string::npos) return ".";
  if (s == 0) return "/";
  return p.substr(0, s);
}

static std::string lower(std::string s)
{
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static bool readAll(const std::string& path, std::string& out)
{
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

static std::string printable(const std::string& s)
{
  std::string o;
  for (unsigned char c : s) {
    if (c == '\t') o += "\\t";
    else if (c < 0x20 || c >= 0x7f) {
      char b[8];
      snprintf(b, sizeof(b), "\\x%02X", c);
      o += b;
    } else
      o += (char)c;
  }
  return o;
}

static std::string joinQuoted(const std::vector<std::string>& v, size_t max = 8)
{
  std::string o;
  for (size_t i = 0; i < v.size() && i < max; ++i) {
    if (i) o += ", ";
    o += "'" + v[i] + "'";
  }
  if (v.size() > max) o += ", ...";
  return o;
}

// semver "2.9.2" -> 20902 ; 0 if absent/unparseable
static int semverNum(const std::string& s)
{
  int a = 0, b = 0, c = 0;
  if (sscanf(s.c_str(), "%d.%d.%d", &a, &b, &c) < 2) return 0;
  return a * 10000 + b * 100 + c;
}

// Undo EdgeTX's writer quoting ("..." with \xHH escapes), mirroring the
// parser's ps_ValQuo / ps_ValEsc* states.
static std::string unquote(const std::string& v)
{
  if (v.size() < 2 || v.front() != '"') return v;
  std::string o;
  for (size_t i = 1; i < v.size(); ++i) {
    char c = v[i];
    if (c == '"') continue;
    if (c == '\\' && i + 1 < v.size()) {
      char e = v[++i];
      switch (e) {
        case 'b': o += '\b'; break;
        case 't': o += '\t'; break;
        case 'n': o += '\n'; break;
        case 'f': o += '\f'; break;
        case 'r': o += '\r'; break;
        case 'x':
          if (i + 2 < v.size()) {
            o += (char)strtol(v.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
          }
          break;
        default: o += e;
      }
      continue;
    }
    o += c;
  }
  return o;
}

// YAML_ENUM gained an is_active callback after 2.10
template <class E>
static auto enumHasActive(const E& e, int) -> decltype(e.is_active, bool())
{
  return e.is_active != nullptr;
}
template <class E>
static bool enumHasActive(const E&, long)
{
  return false;
}

static bool textWriterFn(void* opaque, const char* str, size_t len)
{
  ((std::string*)opaque)->append(str, len);
  return true;
}

// --------------------------------------------------------------------------
// Instrumented parse session

namespace {

// Extract the buffer argument type of a YamlParserCalls member, because it
// differs between EdgeTX versions (char* in 2.10, const char* in 2.11+).
template <class F> struct BufArg;
template <class R, class A0, class A1, class A2>
struct BufArg<R (*)(A0, A1, A2)> {
  using type = A1;
};
using FindBuf = BufArg<decltype(YamlParserCalls::find_node)>::type;
using SetBuf = BufArg<decltype(YamlParserCalls::set_attr)>::type;

struct Level {
  std::string key;
  int line = 0;
  bool keyFound = false;
  bool keyIsIdx = false;
  bool idxInvalid = false;
  bool keyIsContainer = false;
  const YamlNode* keyNode = nullptr;
  const YamlNode* container = nullptr;  // logical container at this level
  int elmt = -1;                         // element index in a dash list
  bool suppressed = false;               // inside an already-reported subtree
  std::string scalarParent;              // parent key is a scalar (bad nesting)
};

// Identity of one stored value: schema node + storage bit offset + the
// walker's element-index chain. The chain matters for size-0 arrays (e.g.
// 2.11+ colour-screen widget options), whose elements share a bit offset.
using ValKey = std::tuple<const YamlNode*, uint32_t, std::string>;

static std::string elmtChain(YamlTreeWalker& w)
{
  std::string c;
  for (uint8_t i = 0; i < NODE_STACK_DEPTH; ++i) {
    c += std::to_string(w.getElmts(i));
    c += ',';
  }
  return c;
}

struct InputRec {
  int line = 0;
  std::string path;
  std::string elementPath;  // path of the innermost list/array element
  std::string key;
  std::string value;
  const YamlNode* node = nullptr;
  uint32_t bitofs = 0;
  ValKey vkey;
  std::vector<const YamlNode*> containers;  // root .. innermost
  std::string gatePath;   // innermost is_active/union container instance
  bool gatedByUnion = false;
  bool dynamic = false;   // inside a size-0 (non-flat storage) container
  bool flagged = false;   // tier 1 already reported it
  bool overridden = false;  // a later duplicate of the same key wins
};

struct ParseSession {
  YamlTreeWalker walker;
  const YamlParserCalls* stock = nullptr;
  const SchemaIndex* schema = nullptr;
  const Options* opt = nullptr;
  FileResult* res = nullptr;

  std::vector<Level> lv;
  int level = 0;
  int line = 0;
  std::vector<InputRec> recs;
  std::map<ValKey, size_t> seen;  // dup keys -> index in recs

  Level& L(int i) {
    if ((int)lv.size() <= i) lv.resize(i + 1);
    return lv[i];
  }

  std::string pathAt(int upto) {
    std::string s;
    for (int i = 0; i <= upto && i < (int)lv.size(); ++i) {
      const Level& x = lv[i];
      if (x.keyIsIdx) {
        s += "[" + x.key + "]";
        continue;
      }
      if (x.elmt >= 0) s += "[" + std::to_string(x.elmt) + "]";
      if (!s.empty()) s += ".";
      s += x.key;
    }
    return s;
  }

  std::string elementPathAt(int upto) {
    std::string p = pathAt(upto);
    auto b = p.find_last_of(']');
    return b == std::string::npos ? "" : p.substr(0, b + 1);
  }

  void add(Severity sev, const char* rule, int line_, const std::string& path,
           const std::string& msg, const std::string& hint = "") {
    Finding f;
    f.sev = sev;
    f.rule = rule;
    f.line = line_;
    f.path = path;
    f.message = msg;
    f.hint = hint;
    res->findings.push_back(std::move(f));
  }

  void reportMiss(const std::string& key) {
    Level& cur = L(level);
    std::string path = pathAt(level);
    std::string shown = printable(key);

    if (!cur.scalarParent.empty()) {
      add(Severity::Error, "wrong-nesting", line, path,
          "'" + shown + "' is nested under '" + cur.scalarParent +
              "', which is a plain value; the radio ignores it",
          "check the indentation");
      return;
    }

    std::string hint;
    if (!key.empty() && key[0] == '#') {
      hint = "EdgeTX's parser has no comment support; '#' lines are read as keys";
    } else if (key.find('\t') != std::string::npos) {
      hint = "tabs are not indentation for EdgeTX's parser; use spaces";
    } else {
      auto sib = childTags(cur.container);
      auto near = closest(key, sib);
      std::vector<std::string> elsewhere = schema->pathsFor(key);
      if (!near.empty())
        hint = "did you mean " + joinQuoted(near) + "?";
      if (!elsewhere.empty()) {
        if (!hint.empty()) hint += " ";
        hint += "'" + key + "' is valid at " + joinQuoted(elsewhere, 4) +
                " (wrong nesting?)";
      }
    }
    Severity sev = opt->warnUnknown ? Severity::Warning : Severity::Error;
    add(sev, "unknown-key", line, path,
        "unknown key '" + shown + "': the radio silently ignores it" +
            (cur.keyIsContainer ? "" : ""),
        hint);
  }

  void validateValue(InputRec& r) {
    const YamlNode* a = r.node;
    const std::string& v = r.value;
    switch (a->type) {
      case YDT_ENUM: {
        bool ok = false;
        std::vector<std::string> choices;
        for (const YamlIdStr* c = a->u._enum.choices; c && c->str; ++c) {
          choices.emplace_back(c->str);
          if (v == c->str) ok = true;
        }
        if (!ok) {
          r.flagged = true;
          auto near = closest(v, choices);
          std::string hint = near.empty()
                                 ? "valid values: " + joinQuoted(choices, 12)
                                 : "did you mean " + joinQuoted(near) + "?";
          add(Severity::Error, "bad-value", r.line, r.path,
              "'" + printable(v) + "' is not a valid value for '" + r.key +
                  "'; the radio stores the default instead",
              hint);
        }
        break;
      }
      case YDT_SIGNED:
      case YDT_UNSIGNED: {
        if (a->u._cust.cust_to_uint) {
          // EdgeTX custom readers have no error path: an unknown token maps
          // to some fallback. Compare against a token nothing can parse; if
          // the result is identical and does not print back as the input,
          // the radio did not understand the value.
          static const char junk[] = "\x7f?";
          uint32_t got = a->u._cust.cust_to_uint(a, v.c_str(), (uint8_t)v.size());
          uint32_t bad = a->u._cust.cust_to_uint(a, junk, sizeof(junk) - 1);
          if (got == bad && a->u._cust.uint_to_cust) {
            std::string txt;
            a->u._cust.uint_to_cust(a, bad, textWriterFn, &txt);
            if (lower(unquote(txt)) != lower(v)) {
              r.flagged = true;
              add(Severity::Error, "bad-value", r.line, r.path,
                  "'" + printable(v) + "' is not understood for '" + r.key +
                      "'; the radio stores it as '" + unquote(txt) + "'");
            }
          }
          break;
        }
        bool isSigned = a->type == YDT_SIGNED;
        static const std::regex sre("-?[0-9]+"), ure("[0-9]+");
        if (!std::regex_match(v, isSigned ? sre : ure)) {
          r.flagged = true;
          add(Severity::Error, "bad-value", r.line, r.path,
              "'" + printable(v) + "' is not a valid " +
                  (isSigned ? "integer" : "unsigned integer") + " for '" +
                  r.key + "'; the radio reads it as " +
                  (isSigned ? std::to_string(yaml_str2int(v.c_str(), (uint8_t)v.size()))
                            : std::to_string(yaml_str2uint(v.c_str(), (uint8_t)v.size()))));
          break;
        }
        long long n = strtoll(v.c_str(), nullptr, 10);
        long long lo = isSigned ? -(1LL << (a->size - 1)) : 0;
        long long hi = isSigned ? (1LL << (a->size - 1)) - 1 : (1LL << a->size) - 1;
        if (a->size < 32 && (n < lo || n > hi)) {
          r.flagged = true;
          add(Severity::Error, "out-of-range", r.line, r.path,
              v + " does not fit '" + r.key + "' (" + std::to_string(a->size) +
                  "-bit field, " + std::to_string(lo) + ".." +
                  std::to_string(hi) + "); the radio truncates it");
        }
        break;
      }
      case YDT_STRING: {
        size_t max = a->size >> 3;
        if (v.size() > max) {
          r.flagged = true;
          add(Severity::Warning, "string-truncated", r.line, r.path,
              "'" + r.key + "' is " + std::to_string(v.size()) +
                  " chars; the radio keeps only the first " +
                  std::to_string(max));
        }
        break;
      }
      default:
        break;
    }
  }
};

ParseSession* g_sess = nullptr;  // parser callbacks only get the ctx we pass

bool w_to_parent(void* ctx)
{
  auto s = (ParseSession*)ctx;
  bool r = s->stock->to_parent(&s->walker);
  if (r && s->level > 0) s->level--;
  return r;
}

bool w_to_child(void* ctx)
{
  auto s = (ParseSession*)ctx;
  Level& cur = s->L(s->level);
  const YamlNode* logical = nullptr;
  if (cur.keyFound && !cur.idxInvalid) {
    logical = cur.keyIsIdx ? s->walker.getNode() : cur.keyNode;
  }
  bool r = s->stock->to_child(&s->walker);
  if (r) {
    s->level++;
    Level nl;
    nl.container = (cur.keyFound && cur.keyIsContainer && !cur.idxInvalid) ? logical : nullptr;
    nl.suppressed = cur.suppressed || !cur.keyFound || cur.idxInvalid ||
                    !cur.scalarParent.empty();
    if (cur.keyFound && !cur.keyIsContainer && !cur.suppressed &&
        cur.scalarParent.empty())
      nl.scalarParent = s->pathAt(s->level - 1);
    if (!nl.scalarParent.empty()) nl.suppressed = false;
    s->L(s->level) = nl;
  }
  return r;
}

bool w_to_next_elmt(void* ctx)
{
  auto s = (ParseSession*)ctx;
  return s->stock->to_next_elmt(&s->walker);
}

bool w_find_node(void* ctx, FindBuf buf, uint8_t len)
{
  auto s = (ParseSession*)ctx;
  Level& cur = s->L(s->level);
  // keep container/suppression, reset per-key state
  cur.key.assign(buf, len);
  cur.line = s->line;
  cur.keyFound = cur.keyIsIdx = cur.idxInvalid = cur.keyIsContainer = false;
  cur.keyNode = nullptr;
  cur.elmt = -1;

  bool underIdx = s->level > 0 && s->lv[s->level - 1].keyIsIdx;
  if (isDashList(cur.container) && !underIdx) cur.elmt = s->walker.getElmts();

  bool r = s->stock->find_node(&s->walker, buf, len);
  if (r) {
    const YamlNode* a = s->walker.getAttr();
    cur.keyFound = true;
    cur.keyNode = a;
    if (a && a->type == YDT_IDX) {
      cur.keyIsIdx = true;
      cur.keyIsContainer = true;
      cur.elmt = -1;
      // EdgeTX marks out-of-range indexes invalid and silently drops the
      // whole element; replicate the check to report it.
      std::string k = cur.key;
      uint32_t i = a->u._cust_idx.read
                       ? a->u._cust_idx.read(&s->walker, k.c_str(), (uint8_t)k.size())
                       : yaml_str2uint(k.c_str(), (uint8_t)k.size());
      const YamlNode* arr = s->walker.getNode();
      if (arr && i >= arr->elmts) {
        cur.idxInvalid = true;
        if (!cur.suppressed)
          s->add(Severity::Error, "bad-index", s->line, s->pathAt(s->level),
                 "index '" + printable(k) + "' is not valid here (max " +
                     std::to_string(arr->elmts) +
                     " entries); the radio drops this whole entry");
      }
    } else if (a && (a->type == YDT_ARRAY || a->type == YDT_UNION)) {
      cur.keyIsContainer = true;
    }
  } else if (!cur.suppressed) {
    s->reportMiss(cur.key);
  }
  return r;
}

void w_set_attr(void* ctx, SetBuf buf, uint16_t len)
{
  auto s = (ParseSession*)ctx;
  Level& cur = s->L(s->level);
  const YamlNode* a = s->walker.getAttr();
  if (a && (a->type == YDT_ARRAY || a->type == YDT_UNION) && cur.keyFound &&
      !cur.suppressed) {
    std::string v(buf, len);
    if (v != "~" && v != "null")
      s->add(Severity::Warning, "ignored-value", s->line, s->pathAt(s->level),
             "'" + cur.key + "' is a section, not a value; the radio ignores '" +
                 printable(v) + "'");
  } else if (a && a->type != YDT_IDX && cur.keyFound && !cur.suppressed) {
    InputRec r;
    r.line = s->line;
    r.key = cur.key;
    r.path = s->pathAt(s->level);
    r.elementPath = s->elementPathAt(s->level);
    r.value.assign(buf, len);
    r.node = a;
    r.bitofs = s->walker.getBitOffset();
    r.vkey = ValKey{a, r.bitofs, elmtChain(s->walker)};
    for (int i = 0; i <= s->level; ++i) {
      const YamlNode* c = s->lv[i].container;
      if (!c) continue;
      r.containers.push_back(c);
      if (i > 0 && c->size == 0) r.dynamic = true;
      bool gated = c->type == YDT_UNION ||
                   (c->type == YDT_ARRAY && c->u._array.u.is_active);
      if (gated && i > 0) {
        r.gatePath = s->pathAt(i - 1);
        if (s->lv[i].elmt >= 0) r.gatePath += "[" + std::to_string(s->lv[i].elmt) + "]";
        r.gatedByUnion = c->type == YDT_UNION;
      }
    }

    const ValKey& k = r.vkey;
    auto it = s->seen.find(k);
    if (it != s->seen.end()) {
      InputRec& prev = s->recs[it->second];
      prev.overridden = true;
      s->add(Severity::Warning, "duplicate-key", r.line, r.path,
             "'" + r.key + "' is set again; this value overrides line " +
                 std::to_string(prev.line));
    }
    s->seen[k] = s->recs.size();
    if (len > 255 && a->type != YDT_STRING) {
      r.flagged = true;
      s->add(Severity::Error, "bad-value", r.line, r.path,
             "value longer than 255 chars; EdgeTX truncates the length");
    }
    s->validateValue(r);
    s->recs.push_back(std::move(r));
  }
  s->stock->set_attr(&s->walker, buf, len);
}

const YamlParserCalls kWrappedCalls = {w_to_parent, w_to_child, w_to_next_elmt,
                                       w_find_node, w_set_attr};

// --------------------------------------------------------------------------
// Instrumented writer: records (node, bit offset) -> written value

struct WriteSession {
  YamlTreeWalker* walker = nullptr;
  std::string text;
  std::string line;
  std::map<ValKey, std::string> emitted;
};

bool recWriter(void* opaque, const char* str, size_t len)
{
  auto w = (WriteSession*)opaque;
  w->text.append(str, len);
  w->line.append(str, len);
  if (w->line.size() >= 2 && w->line.compare(w->line.size() - 2, 2, "\r\n") == 0) {
    std::string ln = w->line.substr(0, w->line.size() - 2);
    w->line.clear();
    auto sep = ln.find(": ");
    if (sep != std::string::npos && sep + 2 < ln.size()) {
      const YamlNode* a = w->walker->getAttr();
      if (a && a->type != YDT_ARRAY && a->type != YDT_UNION && a->type != YDT_IDX) {
        w->emitted[ValKey{a, w->walker->getBitOffset(), elmtChain(*w->walker)}] =
            unquote(ln.substr(sep + 2));
      }
    }
  }
  return true;
}

bool textWriter(void* opaque, const char* str, size_t len)
{
  ((std::string*)opaque)->append(str, len);
  return true;
}

}  // namespace

// --------------------------------------------------------------------------
// Loading via EdgeTX's own loaders (authoritative state for tier 2)

static std::string makeTempDir()
{
  const char* t = getenv("TMPDIR");
  std::string tmpl = std::string(t && *t ? t : "/tmp") + "/edgetx-lint-XXXXXX";
  std::vector<char> b(tmpl.begin(), tmpl.end());
  b.push_back('\0');
  if (!mkdtemp(b.data())) return "";
  return std::string(b.data());
}

static void rmTree(const std::string& d)
{
  if (d.empty()) return;
  // only ever our own mkdtemp() directory
  (void)unlink((d + "/RADIO/radio.yml").c_str());
  (void)unlink((d + "/model.yml").c_str());
  (void)rmdir((d + "/RADIO").c_str());
  (void)rmdir(d.c_str());
}

static bool copyFile(const std::string& data, const std::string& to)
{
  std::ofstream o(to, std::ios::binary);
  o.write(data.data(), data.size());
  return (bool)o;
}

// returns nullptr on success, EdgeTX error string otherwise
static const char* officialLoad(bool isRadio, const std::string& bytes,
                                std::string& tmp)
{
  tmp = makeTempDir();
  if (tmp.empty()) return "cannot create temp dir";
  if (isRadio) {
    mkdir((tmp + "/RADIO").c_str(), 0700);
    if (!copyFile(bytes, tmp + "/RADIO/radio.yml")) return "cannot write temp file";
    simuFatfsSetPaths(tmp.c_str(), nullptr);
    memset(&g_eeGeneral, 0, sizeof(g_eeGeneral));
    return loadRadioSettingsYaml(false);
  }
  if (!copyFile(bytes, tmp + "/model.yml")) return "cannot write temp file";
  simuFatfsSetPaths(tmp.c_str(), nullptr);
  return readModelYaml("model.yml", (uint8_t*)&g_model, sizeof(g_model), "");
}

static std::string generateText(const YamlNode* root, uint8_t* data,
                                WriteSession* ws)
{
  YamlTreeWalker tree;
  tree.reset(root, data);
  if (ws) {
    ws->walker = &tree;
    tree.generate(recWriter, ws);
    return ws->text;
  }
  std::string out;
  tree.generate(textWriter, &out);
  return out;
}

// --------------------------------------------------------------------------
// Rules needing whole-file context

// Mix/expo weight & offset, and curve values, used EdgeTX <=2.10's
// in_read_weight() GVar encoding: a NUMBER in these ranges meant a global
// variable. From 2.11 (r_sourceNumVal) any number is a literal value.
struct LegacyGvField {
  std::regex path;
  int bits2_10;  // field width in 2.10 (selects GV1_SMALL/GV1_LARGE)
};

static const std::vector<LegacyGvField>& legacyGvFields()
{
  static const std::vector<LegacyGvField> f = {
      {std::regex(R"(^mixData\[\d+\]\.weight$)"), 11},
      {std::regex(R"(^mixData\[\d+\]\.offset$)"), 13},
      {std::regex(R"(^mixData\[\d+\]\.curve\.value$)"), 8},
      {std::regex(R"(^expoData\[\d+\]\.weight$)"), 8},
      {std::regex(R"(^expoData\[\d+\]\.offset$)"), 8},
      {std::regex(R"(^expoData\[\d+\]\.curve\.value$)"), 8},
  };
  return f;
}

// returns GV name ("GV1", "-GV3") if `v` is a 2.10 GVar encoding, else ""
static std::string legacyGvName(long v, int bits)
{
  const long gv1 = bits > 8 ? 1024 : 128;  // GV1_LARGE / GV1_SMALL
  const int maxGv = 9;                     // the 2.10 reader handles GV1..GV9
  if (v >= -gv1 && v <= -gv1 + maxGv - 1) return "GV" + std::to_string(v + gv1 + 1);
  if (v <= gv1 - 1 && v >= gv1 - maxGv) return "-GV" + std::to_string(gv1 - v);
  return "";
}

static void versionTrap(ParseSession& s, const std::string& semver,
                        const std::map<ValKey, std::string>* emitted)
{
  const int fileVer = semverNum(semver);
  const int libVer = ETX_VERSION_NUM;
  const bool libNew = libVer >= 21100;
  if (fileVer == 0) return;  // reported separately
  const bool fileNew = fileVer >= 21100;
  if (libNew == fileNew) return;

  static const std::regex num("-?[0-9]+");
  for (auto& r : s.recs) {
    if (!std::regex_match(r.value, num)) continue;
    for (auto& f : legacyGvFields()) {
      if (!std::regex_match(r.path, f.path)) continue;
      std::string gv = legacyGvName(strtol(r.value.c_str(), nullptr, 10), f.bits2_10);
      if (gv.empty()) break;
      r.flagged = true;
      std::string stored;
      if (emitted) {
        auto it = emitted->find(r.vkey);
        if (it != emitted->end()) stored = it->second;
      }
      if (libNew) {
        s.add(Severity::Error, "version-trap", r.line, r.path,
              "'" + r.key + ": " + r.value + "' was written by EdgeTX " + semver +
                  ", where it means " + gv + ". EdgeTX " + VERSION +
                  " reads it as a plain number" +
                  (stored.empty() ? "" : " and stores " + stored + " (out of range for the field)") +
                  "; no conversion happens (semver is write-only)",
              "replace the number with '" + gv + "', or re-save the model with EdgeTX 2.10 Companion before upgrading");
      } else {
        s.add(Severity::Error, "version-trap", r.line, r.path,
              "'" + r.key + ": " + r.value + "' was written by EdgeTX " + semver +
                  " as a literal value, but EdgeTX " + VERSION + " reads it as " + gv,
              "this file is from a newer EdgeTX; do not load it on " VERSION);
      }
      break;
    }
  }
}

// --------------------------------------------------------------------------

#if ETX_VERSION_NUM < 21100
#include "hal/adc_driver.h"
extern const etx_hal_adc_driver_t simu_adc_driver;
#endif

void backendInit()
{
  simuInit();
#if ETX_VERSION_NUM < 21100
  adcInit(&simu_adc_driver);  // as EdgeTX 2.10's gtests main()
#endif
#if defined(COLORLCD)
  // same as EdgeTX's gtests main(): model defaults touch the LVGL layer
  lcdInitDisplayDriver();
#endif
}

static std::string detectKind(const std::string& path, const std::string& force)
{
  if (!force.empty()) return force;
  std::string b = lower(baseName(path));
  if (b == "radio.yml") return "radio";
  if (b == "labels.yml" || b == "models.yml") return "skipped";
  return "model";
}

FileResult lintFile(const std::string& path, const Options& opt)
{
  FileResult res;
  res.file = path;
  res.kind = detectKind(path, opt.forceType);

  auto addf = [&](Severity sev, const char* rule, int line, const std::string& p,
                  const std::string& msg, const std::string& hint = "") {
    Finding f;
    f.sev = sev;
    f.rule = rule;
    f.line = line;
    f.path = p;
    f.message = msg;
    f.hint = hint;
    res.findings.push_back(std::move(f));
  };

  if (res.kind == "skipped") {
    addf(Severity::Info, "not-linted", 0, "",
         "models list / labels file: not a model or radio settings file");
    return res;
  }

  std::string bytes;
  if (!readAll(path, bytes)) {
    res.loadOk = false;
    addf(Severity::Error, "io-error", 0, "", std::string("cannot read file: ") + strerror(errno));
    return res;
  }

  // Radio settings always carry a top-level 'board:' key (w_board); models never do.
  if (opt.forceType.empty() && res.kind == "model" &&
      (bytes.compare(0, 7, "board: ") == 0 || bytes.find("\nboard: ") != std::string::npos))
    res.kind = "radio";

  const bool isRadio = res.kind == "radio";
  const YamlNode* root = isRadio ? get_radiodata_nodes() : get_modeldata_nodes();

  SchemaIndex schema;
  schema.build(root);

  // ---- readYamlFile(): a leading "checksum: N" line is skipped by the radio
  size_t skip = 0;
  long fileChecksum = -1;
  int firstLine = 1;
  {
    const char* tag = "checksum: ";
    if (bytes.compare(0, strlen(tag), tag) == 0) {
      size_t e = bytes.find_first_of("\r\n");
      if (e == std::string::npos) e = bytes.size();
      fileChecksum = atol(bytes.substr(strlen(tag), e - strlen(tag)).c_str());
      skip = e;
      while (skip < bytes.size() && (bytes[skip] == '\r' || bytes[skip] == '\n')) {
        if (bytes[skip] == '\n') firstLine++;
        skip++;
      }
    }
  }

  // ---- tier 1: instrumented parse into a scratch buffer
  std::vector<uint8_t> scratch(isRadio ? sizeof(RadioData) : sizeof(ModelData), 0);
  ParseSession s;
  s.stock = YamlTreeWalker::get_parser_calls();
  s.schema = &schema;
  s.opt = &opt;
  s.res = &res;
  s.walker.reset(root, scratch.data());
  Level rootLv;
  rootLv.container = root;
  s.lv.push_back(rootLv);

  YamlParser yp;
  yp.init(&kWrappedCalls, &s);
  {
    size_t pos = skip;
    s.line = firstLine - 1;
    while (pos < bytes.size()) {
      size_t nl = bytes.find('\n', pos);
      size_t end = nl == std::string::npos ? bytes.size() : nl + 1;
      s.line++;
      if (end >= bytes.size()) yp.set_eof();
      auto r = yp.parse(bytes.data() + pos, (unsigned)(end - pos));
      if (r != YamlParser::CONTINUE_PARSING) {
        size_t rest = std::count(bytes.begin() + end, bytes.end(), '\n');
        std::string lineText = bytes.substr(pos, end - pos);
        static const std::regex lowerHex(R"(\\x([0-9A-Fa-f]?[a-f]|[a-f][0-9A-Fa-f]))");
        std::string hint;
        if (r == YamlParser::STRING_OVERFLOW)
          hint = "EdgeTX " VERSION " limits values to 256 chars";
        else if (std::regex_search(lineText, lowerHex))
          hint = "EdgeTX " VERSION " only accepts UPPERCASE hex digits in \\x escapes (e.g. \\x1D, not \\x1d)";
        else
          hint = "check indentation depth and escape sequences on this line";
        addf(Severity::Error, "parse-aborted", s.line, s.pathAt(s.level),
             std::string(r == YamlParser::STRING_OVERFLOW ? "value too long: " : "") +
                 "EdgeTX's parser stops reading at this line; the remaining " +
                 std::to_string(rest) + " lines of the file are silently ignored",
             hint);
        break;
      }
      pos = end;
    }
  }

  // semver: a write-only key, but it tells us who wrote the file
  for (auto& r : s.recs)
    if (r.path == "semver") res.semver = r.value;

  // ---- tier 2: authoritative load + write-back + diff
  std::string tmp;
  const char* err = officialLoad(isRadio, bytes, tmp);
  rmTree(tmp);
  if (err) {
    res.loadOk = false;
    addf(Severity::Error, "load-failed", 0, "", std::string("EdgeTX loader failed: ") + err);
  }

  if (isRadio) {
    // radio.yml checksum (sdcard_yaml.cpp loadRadioSettingsYaml)
    if (fileChecksum >= 0 && !(fileChecksum == 0 && bytes.size() > 25)) {
      uint16_t calc = crc16(0, (const uint8_t*)bytes.data() + skip,
                            (uint32_t)(bytes.size() - skip), 0xFFFF);
      if (calc != (uint16_t)fileChecksum) {
        if (!err && g_eeGeneral.manuallyEdited) {
          addf(Severity::Info, "checksum", 1, "checksum",
               "checksum mismatch accepted because manuallyEdited is set");
        } else {
          addf(Severity::Error, "checksum", 1, "checksum",
               "checksum " + std::to_string(fileChecksum) + " does not match content (" +
                   std::to_string(calc) +
                   "): the radio treats radio.yml as corrupted and falls back to the backup/defaults",
               "set 'manuallyEdited: 1' or 'checksum: " + std::to_string(calc) + "'");
        }
      }
    }
    // board check (w_board is write-only, FLAVOUR is what this build writes)
    for (auto& r : s.recs)
      if (r.path == "board" && r.value != FLAVOUR)
        addf(Severity::Error, "wrong-radio", r.line, r.path,
             "file was written by a '" + r.value + "' radio, linted as '" FLAVOUR "'",
             "use --radio matching the file");
  }

  if (!isRadio && res.semver.empty()) {
    addf(Severity::Warning, "no-semver", 0, "semver",
         "no 'semver' key: cannot tell which EdgeTX version wrote this file "
         "(version-dependent value encodings cannot be checked)");
  }

  bool trapDone = false;
  if (!err && !opt.noRoundtrip) {
    WriteSession ws;
    uint8_t* data = isRadio ? (uint8_t*)&g_eeGeneral : (uint8_t*)&g_model;
    generateText(root, data, &ws);

    versionTrap(s, res.semver, &ws.emitted);
    trapDone = true;

    uint8_t* stored = isRadio ? (uint8_t*)&g_eeGeneral : (uint8_t*)&g_model;
    std::map<std::string, std::vector<const InputRec*>> dropped;  // by group
    std::vector<std::string> droppedOrder;
    for (auto& r : s.recs) {
      const YamlNode* n = r.node;
      if (r.overridden) continue;
      if (n->type == YDT_CUSTOM && !n->u._cust_attr.write) {
        if (n->u._cust_attr.read)
          addf(Severity::Info, "legacy-key", r.line, r.path,
               "'" + r.key + "' is a read-only legacy key: understood, converted, "
               "and written back in the current format on next save");
        continue;  // never round-trips by design
      }
      if (n->type == YDT_CUSTOM && !n->u._cust_attr.read) continue;  // write-only (semver, board)
      auto it = ws.emitted.find(r.vkey);
      if (it != ws.emitted.end()) {
        if (r.flagged || lower(it->second) == lower(r.value)) continue;
        static const std::set<std::string> nullish = {"", "0", "NONE", "---", "OFF", "\"\""};
        static const std::regex intRe("-?[0-9]+");
        if (std::regex_match(r.value, intRe) && std::regex_match(it->second, intRe)) {
          addf(Severity::Error, "out-of-range", r.line, r.path,
               r.value + " is out of range for '" + r.key + "'; the radio stores " +
                   it->second + " (and writes that back)");
        } else if (nullish.count(it->second) && !nullish.count(r.value)) {
          addf(Severity::Warning, "value-lost", r.line, r.path,
               "the radio reads '" + printable(r.value) + "' as '" + printable(it->second) +
                   "': the value is not understood and is lost on save");
        } else {
          addf(Severity::Info, "normalized", r.line, r.path,
               "the radio rewrites '" + printable(r.value) + "' as '" +
                   printable(it->second) + "' (same setting, current format)");
        }
        continue;
      }
      // Not written back. Lossless if what the radio stored is zero (the
      // implicit value of an absent key) - the writer omits empty entries.
      bool zero = false;
      if (!r.dynamic && n->type != YDT_CUSTOM && n->size > 0)
        zero = yaml_is_zero(stored, r.bitofs, n->size);
      else if (r.dynamic)
        zero = r.value == "0" || r.value.empty();
      if (zero) continue;

      if (n->type == YDT_ENUM && enumHasActive(n->u._enum, 0)) {
        addf(Severity::Warning, "inactive", r.line, r.path,
             "'" + r.key + "' is not used in this context (gated by another setting) and is dropped on save");
        continue;
      }
      std::string key = !r.gatePath.empty() ? r.gatePath
                        : !r.elementPath.empty() ? r.elementPath : r.path;
      if (!dropped.count(key)) droppedOrder.push_back(key);
      dropped[key].push_back(&r);
    }

    for (auto& key : droppedOrder) {
      auto& v = dropped[key];
      const InputRec* first = v.front();
      std::string keys;
      for (size_t i = 0; i < v.size() && i < 4; ++i) keys += (i ? ", " : "") + v[i]->key;
      if (v.size() > 4) keys += ", ...";
      std::string what = std::to_string(v.size()) + " non-default value" + (v.size() > 1 ? "s" : "") +
                         " (" + keys + ")";
      if (!first->gatePath.empty() && first->gatedByUnion)
        addf(Severity::Warning, "inactive", first->line, key,
             what + " belong to a variant that is not selected; ignored and dropped on save");
      else if (!first->gatePath.empty())
        addf(Severity::Warning, "inactive", first->line, key,
             what + " in a section the radio treats as unused (context-gated); dropped on save");
      else
        addf(Severity::Warning, "dropped", first->line, key,
             what + " not written back by the radio; lost on save");
    }
  }

  if (!trapDone) versionTrap(s, res.semver, nullptr);

  // stable order: by line, then rule
  std::stable_sort(res.findings.begin(), res.findings.end(),
                   [](const Finding& a, const Finding& b) { return a.line < b.line; });
  return res;
}

int dumpDefault(const std::string& what, std::string& out)
{
  if (what == "radio") {
    generalDefault();
    out = generateText(get_radiodata_nodes(), (uint8_t*)&g_eeGeneral, nullptr);
  } else if (what == "model") {
    setModelDefaults(0);
    out = generateText(get_modeldata_nodes(), (uint8_t*)&g_model, nullptr);
  } else {
    fprintf(stderr, "dump-default: expected 'model' or 'radio'\n");
    return 2;
  }
  return 0;
}

int roundtripDump(const std::string& path, const std::string& forceType,
                  std::string& out)
{
  std::string bytes;
  if (!readAll(path, bytes)) {
    fprintf(stderr, "cannot read %s\n", path.c_str());
    return 2;
  }
  bool isRadio = detectKind(path, forceType) == "radio";
  std::string tmp;
  const char* err = officialLoad(isRadio, bytes, tmp);
  rmTree(tmp);
  if (err) {
    fprintf(stderr, "EdgeTX loader failed: %s\n", err);
    return 1;
  }
  out = isRadio
      ? generateText(get_radiodata_nodes(), (uint8_t*)&g_eeGeneral, nullptr)
      : generateText(get_modeldata_nodes(), (uint8_t*)&g_model, nullptr);
  return 0;
}
