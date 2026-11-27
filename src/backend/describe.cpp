// edgetx-lint: `describe` - facts JSON about what the radio LOADS
//
// Every fact comes from the state EdgeTX's own loader leaves in g_model /
// g_eeGeneral (readModelYaml / loadRadioSettingsYaml, via lintFile()). Keys the
// radio ignores, lines after a parse abort, and values it clamps never reach
// that state, so they can't be documented by accident.
//
//  - "raw" tokens are produced by EdgeTX's own YAML writer callbacks for that
//    field (looked up in the compiled schema), i.e. exactly what the radio
//    writes back for the loaded value.
//  - "text" strings come from EdgeTX's UI helpers (getSourceString,
//    getSwitchPositionName, getCurveString, ...), so names match the radio
//    screen, with the radio font's icon glyphs mapped to Unicode or dropped.
//
// Struct layouts differ per EdgeTX version; differences are handled with
// ETX_VERSION_NUM / #ifdef only. Nothing in EdgeTX is modified.

#include "describe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "linter.h"
#include "report.h"

#include "etx.h"
#include "hal/adc_driver.h"
#include "switches.h"
#include "analogs.h"

// ---------------------------------------------------------------- JSON

namespace {

std::string jstr(const std::string& s)
{
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else
          o += (char)c;
    }
  }
  return o + "\"";
}

// Minimal ordered JSON value
struct J {
  enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
  bool b = false;
  std::string s;  // number text or string
  std::vector<std::pair<std::string, J>> kv;
  std::vector<J> a;

  static J null() { return J(); }
  static J boolean(bool v) { J j; j.t = Bool; j.b = v; return j; }
  static J num(long v) { J j; j.t = Num; j.s = std::to_string(v); return j; }
  static J real(double v)
  {
    J j;
    j.t = Num;
    char b[32];
    snprintf(b, sizeof(b), "%.6g", v);
    j.s = b;
    return j;
  }
  static J str(const std::string& v) { J j; j.t = Str; j.s = v; return j; }
  static J arr() { J j; j.t = Arr; return j; }
  static J obj() { J j; j.t = Obj; return j; }

  J& set(const std::string& k, J v)
  {
    for (auto& p : kv)
      if (p.first == k) { p.second = std::move(v); return *this; }
    kv.emplace_back(k, std::move(v));
    return *this;
  }
  J& set(const std::string& k, const std::string& v) { return set(k, str(v)); }
  J& set(const std::string& k, const char* v) { return set(k, str(v)); }
  J& set(const std::string& k, long v) { return set(k, num(v)); }
  J& set(const std::string& k, int v) { return set(k, num(v)); }
  J& set(const std::string& k, bool v) { return set(k, boolean(v)); }
  J& push(J v) { a.push_back(std::move(v)); return *this; }

  void dump(std::string& o, int ind, bool pretty) const
  {
    auto nl = [&](int d) {
      if (!pretty) return;
      o += '\n';
      o.append(d * 2, ' ');
    };
    switch (t) {
      case Null: o += "null"; break;
      case Bool: o += b ? "true" : "false"; break;
      case Num: o += s; break;
      case Str: o += jstr(s); break;
      case Arr:
        if (a.empty()) { o += "[]"; break; }
        o += '[';
        for (size_t i = 0; i < a.size(); ++i) {
          if (i) o += ',';
          nl(ind + 1);
          a[i].dump(o, ind + 1, pretty);
        }
        nl(ind);
        o += ']';
        break;
      case Obj:
        if (kv.empty()) { o += "{}"; break; }
        o += '{';
        for (size_t i = 0; i < kv.size(); ++i) {
          if (i) o += ',';
          nl(ind + 1);
          o += jstr(kv[i].first);
          o += pretty ? ": " : ":";
          kv[i].second.dump(o, ind + 1, pretty);
        }
        nl(ind);
        o += '}';
        break;
    }
  }
};

// ---------------------------------------------------------------- text

// The radio font's icon glyphs (translations/untranslated.h CHAR_*) are
// encoded as "\302\2xx". Arrows become Unicode; type icons are dropped
// (the "kind" field says what a reference is).
std::string uiText(const char* s)
{
  std::string o;
  if (!s) return o;
  for (size_t i = 0; s[i]; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0302 && s[i + 1]) {
      unsigned char g = (unsigned char)s[i + 1];
      if (g >= 0200 && g <= 0237) {
        ++i;
        switch (g) {
          case 0200: o += "→"; break;  // right
          case 0201: o += "←"; break;  // left
          case 0202: o += "↑"; break;  // up
          case 0203: o += "↓"; break;  // down
          case 0210: o += "Δ"; break;  // delta
          default: break;                   // type icon
        }
        continue;
      }
    }
    if (c >= 0x80 || c < 0x20) {
      // valid UTF-8 passes through; anything else is escaped
      int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
      bool ok = n > 0;
      for (int k = 1; ok && k <= n; ++k)
        ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
      if (ok) {
        o.append(s + i, n + 1);
        i += n;
        continue;
      }
      char b[8];
      snprintf(b, sizeof(b), "\\x%02X", c);
      o += b;
      continue;
    }
    o += (char)c;
  }
  // icons can leave a leading space in some builds
  while (!o.empty() && o[0] == ' ') o.erase(0, 1);
  return o;
}

std::string fixedStr(const char* p, size_t n)
{
  size_t l = strnlen(p, n);
  std::string s(p, l);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return uiText(s.c_str());
}
#define FSTR(a) fixedStr((a), sizeof(a))

// ---------------------------------------------------------------- schema access

const YamlNode* childArray(const YamlNode* container, const char* tag)
{
  for (const YamlNode* n = container->u._array.child; n && n->type != YDT_NONE; ++n)
    if (n->tag && !strcmp(n->tag, tag)) return n;
  return nullptr;
}

// Find attribute `tag` among `container`'s children; bit offset relative to
// the start of one element of `container`. Mirrors YamlTreeWalker::toNextAttr.
const YamlNode* findAttr(const YamlNode* container, const char* tag, uint32_t& ofs,
                         uint32_t base = 0)
{
  uint32_t o = base;
  for (const YamlNode* n = container->u._array.child; n && n->type != YDT_NONE; ++n) {
    if (n->type == YDT_UNION && (!n->tag || !n->tag[0])) {
      const YamlNode* r = findAttr(n, tag, ofs, o);
      if (r) return r;
    } else if (n->tag && !strcmp(n->tag, tag)) {
      ofs = o;
      return n;
    }
    if (container->type == YDT_UNION) continue;  // members overlap
    o += n->type == YDT_ARRAY ? (uint32_t)n->elmts * n->size : n->size;
  }
  return nullptr;
}

bool collect(void* opaque, const char* str, size_t len)
{
  ((std::string*)opaque)->append(str, len);
  return true;
}

std::string cleanToken(std::string s)
{
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

// What EdgeTX's writer emits for attribute `tag` of the element at `elem`
// (a pointer to one struct described by `container`).
std::string tok(const YamlNode* container, const char* tag, const void* elem)
{
  if (!container) return "";
  uint32_t ofs = 0;
  const YamlNode* n = findAttr(container, tag, ofs);
  if (!n) return "";
  uint8_t* p = (uint8_t*)elem + (ofs >> 3);
  uint32_t bo = ofs & 7;
  std::string s;
  switch (n->type) {
    case YDT_CUSTOM:
      if (n->u._cust_attr.write) {
        YamlTreeWalker w;  // not used by the writers we call; passed for safety
        n->u._cust_attr.write(&w, p, bo, collect, &s);
      }
      break;
    case YDT_SIGNED:
    case YDT_UNSIGNED: {
      uint32_t i = yaml_get_bits(p, bo, n->size);
      if (n->u._cust.uint_to_cust)
        n->u._cust.uint_to_cust(n, i, collect, &s);
      else if (n->type == YDT_SIGNED)
        s = std::to_string(yaml_to_signed(i, n->size));
      else
        s = std::to_string(i);
      break;
    }
    case YDT_ENUM: {
      uint32_t i = yaml_get_bits(p, bo, n->size);
      const char* e = yaml_output_enum(i, n->u._enum.choices);
      if (!e) e = yaml_output_enum(yaml_to_signed(i, n->size), n->u._enum.choices);
      s = e ? e : std::to_string(i);
      break;
    }
    case YDT_STRING:
      s = fixedStr((const char*)p, n->size >> 3);
      break;
    default:
      break;
  }
  return cleanToken(s);
}

struct Schema {
  const YamlNode* model = nullptr;
  const YamlNode* mix = nullptr;
  const YamlNode* expo = nullptr;
  const YamlNode* limit = nullptr;
  const YamlNode* timer = nullptr;
  const YamlNode* fm = nullptr;
  const YamlNode* ls = nullptr;
  const YamlNode* cfn = nullptr;
  const YamlNode* module = nullptr;
  const YamlNode* trainer = nullptr;
  const YamlNode* gvar = nullptr;
  const YamlNode* swash = nullptr;
  const YamlNode* rfAlarms = nullptr;

  // a struct member of an element node (e.g. mixData -> curve)
  static const YamlNode* sub(const YamlNode* c, const char* tag)
  {
    uint32_t o;
    return c ? findAttr(c, tag, o) : nullptr;
  }

  void init()
  {
    model = get_modeldata_nodes();
    mix = childArray(model, "mixData");
    expo = childArray(model, "expoData");
    limit = childArray(model, "limitData");
    timer = childArray(model, "timers");
    fm = childArray(model, "flightModeData");
    ls = childArray(model, "logicalSw");
    cfn = childArray(model, "customFn");
    module = childArray(model, "moduleData");
    trainer = childArray(model, "trainerData");
    gvar = childArray(model, "gvars");
    swash = childArray(model, "swashR");
    rfAlarms = childArray(model, "rfAlarms");
  }
};

Schema S;

// ---------------------------------------------------------------- references

J switchRef(swsrc_t sw, const std::string& raw)
{
  J j = J::obj();
  j.set("raw", raw);
  if (sw == SWSRC_NONE) {
    j.set("kind", "none");
    j.set("text", "");
    return j;
  }
  j.set("text", uiText(getSwitchPositionName(sw)));
  bool neg = sw < 0;
  swsrc_t a = neg ? -sw : sw;
  if (neg) j.set("negated", true);
  if (a >= SWSRC_FIRST_SWITCH && a <= SWSRC_LAST_SWITCH) {
    div_t si = switchInfo(a);
    char name[32] = {0};
    getSwitchName(name, si.quot);
    static const char* pos[] = {"up", "middle", "down"};
    j.set("kind", "switch");
    j.set("switch", uiText(name));
    j.set("position", si.rem >= 0 && si.rem <= 2 ? pos[si.rem] : "?");
  } else if (a >= SWSRC_FIRST_MULTIPOS_SWITCH && a <= SWSRC_LAST_MULTIPOS_SWITCH) {
    j.set("kind", "multipos");
    j.set("position", (long)((a - SWSRC_FIRST_MULTIPOS_SWITCH) % XPOTS_MULTIPOS_COUNT + 1));
  } else if (a >= SWSRC_FIRST_TRIM && a <= SWSRC_LAST_TRIM) {
    j.set("kind", "trim");
  } else if (a >= SWSRC_FIRST_LOGICAL_SWITCH && a <= SWSRC_LAST_LOGICAL_SWITCH) {
    j.set("kind", "logical");
    j.set("index", (long)(a - SWSRC_FIRST_LOGICAL_SWITCH + 1));
  } else if (a == SWSRC_ON) {
    j.set("kind", "on");
  } else if (a == SWSRC_ONE) {
    j.set("kind", "one");
  } else if (a >= SWSRC_FIRST_FLIGHT_MODE && a <= SWSRC_LAST_FLIGHT_MODE) {
    j.set("kind", "flight_mode");
    j.set("index", (long)(a - SWSRC_FIRST_FLIGHT_MODE));
  } else if (a >= SWSRC_FIRST_SENSOR && a <= SWSRC_LAST_SENSOR) {
    j.set("kind", "telemetry");
  } else {
    j.set("kind", "other");
  }
  return j;
}

const char* sourceKind(mixsrc_t a)
{
  if (a == MIXSRC_NONE) return "none";
  if (a >= MIXSRC_FIRST_INPUT && a <= MIXSRC_LAST_INPUT) return "input";
#if defined(LUA_INPUTS)
  if (a >= MIXSRC_FIRST_LUA && a <= MIXSRC_LAST_LUA) return "lua";
#endif
  if (a >= MIXSRC_FIRST_STICK && a <= MIXSRC_LAST_STICK) return "stick";
  if (a >= MIXSRC_FIRST_POT && a <= MIXSRC_LAST_POT) return "pot";
  if (a == MIXSRC_MIN || a == MIXSRC_MAX) return "constant";
  if (a >= MIXSRC_FIRST_HELI && a <= MIXSRC_LAST_HELI) return "heli";
  if (a >= MIXSRC_FIRST_TRIM && a <= MIXSRC_LAST_TRIM) return "trim";
  if (a >= MIXSRC_FIRST_SWITCH && a <= MIXSRC_LAST_SWITCH) return "switch";
  if (a >= MIXSRC_FIRST_LOGICAL_SWITCH && a <= MIXSRC_LAST_LOGICAL_SWITCH) return "logical";
  if (a >= MIXSRC_FIRST_TRAINER && a <= MIXSRC_LAST_TRAINER) return "trainer";
  if (a >= MIXSRC_FIRST_CH && a <= MIXSRC_LAST_CH) return "channel";
  if (a >= MIXSRC_FIRST_GVAR && a <= MIXSRC_LAST_GVAR) return "gvar";
  if (a >= MIXSRC_FIRST_TIMER && a <= MIXSRC_LAST_TIMER) return "timer";
  if (a >= MIXSRC_FIRST_TELEM) return "telemetry";
  return "other";
}

J sourceRef(mixsrc_t src, const std::string& raw)
{
  J j = J::obj();
  j.set("raw", raw);
  mixsrc_t a = src < 0 ? -src : src;
  j.set("kind", sourceKind(a));
  if (src < 0) j.set("negated", true);
  j.set("text", src == MIXSRC_NONE ? std::string() : uiText(getSourceString(src)));
  if (a >= MIXSRC_FIRST_INPUT && a <= MIXSRC_LAST_INPUT)
    j.set("index", (long)(a - MIXSRC_FIRST_INPUT + 1));
  else if (a >= MIXSRC_FIRST_CH && a <= MIXSRC_LAST_CH)
    j.set("index", (long)(a - MIXSRC_FIRST_CH + 1));
  else if (a >= MIXSRC_FIRST_GVAR && a <= MIXSRC_LAST_GVAR)
    j.set("index", (long)(a - MIXSRC_FIRST_GVAR + 1));
  else if (a >= MIXSRC_FIRST_LOGICAL_SWITCH && a <= MIXSRC_LAST_LOGICAL_SWITCH)
    j.set("index", (long)(a - MIXSRC_FIRST_LOGICAL_SWITCH + 1));
  return j;
}

std::string gvarName(int idx0)
{
  if (idx0 < 0 || idx0 >= MAX_GVARS) return "";
  return FSTR(g_model.gvars[idx0].name);
}

// A numeric field that may instead hold a global variable (<= 2.10, encoded
// in the number range and written as "GVn"/"-GVn") or any source (>= 2.11,
// SourceNumVal). `unit` is appended to literal values in "text".
J valueRef(const YamlNode* container, const char* tag, const void* elem, const char* unit,
           long add = 0, double scale = 1.0)
{
  std::string raw = tok(container, tag, elem);
  J j = J::obj();
  j.set("raw", raw);
  static const std::regex num(R"(-?[0-9]+)");
  static const std::regex gv(R"((-?)GV([0-9]+))");
  std::smatch m;
  if (std::regex_match(raw, num)) {
    double v = add + strtol(raw.c_str(), nullptr, 10) * scale;
    j.set("value", J::real(v));
    char b[48];
    snprintf(b, sizeof(b), "%.6g%s", v, unit);
    j.set("text", b);
    return j;
  }
#if ETX_VERSION_NUM >= 21100
  {
    uint32_t ofs = 0;
    const YamlNode* n = findAttr(container, tag, ofs);
    if (n && n->size == 11) {
      SourceNumVal v;
      v.rawValue = yaml_get_bits((uint8_t*)elem + (ofs >> 3), ofs & 7, n->size);
      if (v.isSource) {
        mixsrc_t src = v.value;
        J s = sourceRef(src, raw);
        mixsrc_t a = src < 0 ? -src : src;
        if (a >= MIXSRC_FIRST_GVAR && a <= MIXSRC_LAST_GVAR) {
          int i = a - MIXSRC_FIRST_GVAR;
          j.set("gvar", (long)(i + 1));
          if (src < 0) j.set("negated", true);
          std::string nm = gvarName(i);
          j.set("text", std::string(src < 0 ? "-" : "") + "GV" + std::to_string(i + 1) +
                            (nm.empty() ? "" : " (" + nm + ")"));
        } else {
          j.set("text", s.kv.size() ? uiText(getSourceString(src)) : raw);
        }
        j.set("source", s);
        return j;
      }
    }
  }
#endif
  if (std::regex_match(raw, m, gv)) {
    int i = atoi(m[2].str().c_str()) - 1;
    bool neg = !m[1].str().empty();
    j.set("gvar", (long)(i + 1));
    if (neg) j.set("negated", true);
    std::string nm = gvarName(i);
    j.set("text", std::string(neg ? "-" : "") + "GV" + std::to_string(i + 1) +
                      (nm.empty() ? "" : " (" + nm + ")"));
    return j;
  }
  j.set("text", raw);
  return j;
}

// flightModes bitmask: bit i set = disabled in flight mode i
J fmMask(uint32_t mask)
{
  J j = J::obj();
  J on = J::arr();
  std::string txt;
  int n = 0;
  for (int i = 0; i < MAX_FLIGHT_MODES; ++i)
    if (!(mask & (1u << i))) {
      on.push(J::num(i));
      txt += std::string(n++ ? ",FM" : "FM") + std::to_string(i);
    }
  j.set("mask", (long)mask);
  j.set("active_in", on);
  j.set("text", mask == 0 ? std::string("all") : n == 0 ? std::string("none") : txt);
  return j;
}

J curveRef(const YamlNode* container, const void* curveElem, const CurveRef& c)
{
  const YamlNode* cn = Schema::sub(container, "curve");
  J j = J::obj();
  static const char* names[] = {"diff", "expo", "func", "custom"};
  j.set("type", c.type < 4 ? names[c.type] : "unknown");
  if (c.type == CURVE_REF_DIFF || c.type == CURVE_REF_EXPO) {
    J v = valueRef(cn, "value", curveElem, "%");
    std::string raw, vt;
    for (auto& p : v.kv) {
      if (p.first == "raw") raw = p.second.s;
      if (p.first == "text") vt = p.second.s;
    }
    j.set("value", v);
    j.set("text", raw == "0" ? std::string() : std::string(c.type ? "Expo " : "Diff ") + vt);
  } else if (c.type == CURVE_REF_FUNC) {
    j.set("value", (long)c.value);
    char b[32] = {0};
    getStringAtIndex(b, STR_VCURVEFUNC, c.value);
    j.set("text", uiText(b));
  } else if (c.type == CURVE_REF_CUSTOM) {
    j.set("value", (long)c.value);
    j.set("text", uiText(getCurveString(c.value)));
  }
  return j;
}

// ---------------------------------------------------------------- multi

struct MultiProto {
  std::string name;
  std::vector<std::string> subs;
};

std::map<int, MultiProto> loadMulti(const std::string& path)
{
  std::map<int, MultiProto> t;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    std::vector<std::string> parts;
    std::stringstream ss(line);
    std::string p;
    while (std::getline(ss, p, ',')) parts.push_back(p);
    if (parts.size() < 2 || parts[0].empty()) continue;
    if (!std::all_of(parts[0].begin(), parts[0].end(), ::isdigit)) continue;
    MultiProto m;
    m.name = parts[1];
    m.subs.assign(parts.begin() + 2, parts.end());
    t[atoi(parts[0].c_str())] = m;
  }
  return t;
}

// ---------------------------------------------------------------- sections

J lintJson(const FileResult& r)
{
  J j = J::obj();
  int e = 0, w = 0, i = 0;
  J fs = J::arr();
  for (auto& f : r.findings) {
    if (f.sev == Severity::Error) e++;
    else if (f.sev == Severity::Warning) w++;
    else i++;
    J x = J::obj();
    x.set("severity", severityName(f.sev));
    x.set("rule", f.rule);
    x.set("line", f.line);
    x.set("path", f.path);
    x.set("message", f.message);
    if (!f.hint.empty()) x.set("hint", f.hint);
    fs.push(x);
  }
  j.set("errors", e);
  j.set("warnings", w);
  j.set("infos", i);
  j.set("findings", fs);
  return j;
}

J modules(const std::map<int, MultiProto>& multi, bool haveMulti)
{
  J arr = J::arr();
  for (int i = 0; i < NUM_MODULES; ++i) {
    const ModuleData& md = g_model.moduleData[i];
    J m = J::obj();
    m.set("index", i);
    m.set("slot", i == 0 ? "internal" : i == 1 ? "external" : "other");
    std::string type = tok(S.module, "type", &md);
    m.set("type", type);
    if (md.type == MODULE_TYPE_NONE) {
      m.set("enabled", false);
      arr.push(m);
      continue;
    }
    m.set("enabled", true);
    std::string sub = tok(S.module, "subType", &md);
    J proto = J::obj();
    proto.set("raw", sub);
    std::string text;
    if (md.type == MODULE_TYPE_MULTIMODULE) {
      int p = -1, s = -1;
      if (sscanf(sub.c_str(), "%d,%d", &p, &s) == 2) {
        proto.set("protocol", (long)p);
        proto.set("subtype", (long)s);
        auto it = multi.find(p);
        if (it != multi.end()) {
          text = it->second.name;
          if (!it->second.subs.empty())
            text += " / " + (s >= 0 && s < (int)it->second.subs.size() ? it->second.subs[s]
                                                                        : "subtype " + std::to_string(s));
        } else if (haveMulti) {
          text = "unknown protocol " + std::to_string(p);
        }
      }
    } else if (md.type == MODULE_TYPE_CROSSFIRE) {
      text = "CRSF";
    }
    proto.set("text", text);
    m.set("protocol", proto);
    std::string cs = tok(S.module, "channelsStart", &md);
    std::string cc = tok(S.module, "channelsCount", &md);
    J ch = J::obj();
    long start = atol(cs.c_str()) + 1;
    long count = atol(cc.c_str());
    ch.set("start", start);
    ch.set("count", count);
    ch.set("text", "CH" + std::to_string(start) + "-CH" + std::to_string(start + count - 1));
    m.set("channels", ch);
    m.set("failsafe", tok(S.module, "failsafeMode", &md));
    arr.push(m);
  }
  return arr;
}

J timers()
{
  J arr = J::arr();
  for (int i = 0; i < MAX_TIMERS; ++i) {
    const TimerData& t = g_model.timers[i];
    if (t.mode == TMRMODE_OFF) continue;
    J j = J::obj();
    j.set("index", i + 1);
    j.set("name", FSTR(t.name));
    j.set("mode", tok(S.timer, "mode", &t));
    j.set("switch", switchRef(t.swtch, tok(S.timer, "swtch", &t)));
    j.set("start_s", (long)t.start);
    j.set("countdown_beep", tok(S.timer, "countdownBeep", &t));
    j.set("minute_beep", (bool)t.minuteBeep);
    j.set("persistent", tok(S.timer, "persistent", &t));
    arr.push(j);
  }
  return arr;
}

J flightModes()
{
  J arr = J::arr();
  for (int i = 0; i < MAX_FLIGHT_MODES; ++i) {
    const FlightModeData& f = g_model.flightModeData[i];
    std::string name = FSTR(f.name);
    if (i > 0 && f.swtch == SWSRC_NONE && name.empty()) continue;
    J j = J::obj();
    j.set("index", i);
    j.set("name", name);
    j.set("switch", switchRef(i == 0 ? SWSRC_NONE : f.swtch, i == 0 ? "" : tok(S.fm, "swtch", &f)));
    j.set("fade_in_s", J::real(f.fadeIn / 10.0));
    j.set("fade_out_s", J::real(f.fadeOut / 10.0));
    arr.push(j);
  }
  return arr;
}

J inputs()
{
  J arr = J::arr();
  for (int i = 0; i < MAX_EXPOS; ++i) {
    const ExpoData& e = g_model.expoData[i];
    if (!EXPO_VALID(&e)) break;
    J j = J::obj();
    j.set("line", i + 1);
    j.set("input", (long)e.chn + 1);
    j.set("input_name", FSTR(g_model.inputNames[e.chn]));
    j.set("name", FSTR(e.name));
    j.set("source", sourceRef(e.srcRaw, tok(S.expo, "srcRaw", &e)));
    j.set("weight", valueRef(S.expo, "weight", &e, "%"));
    j.set("offset", valueRef(S.expo, "offset", &e, "%"));
    j.set("curve", curveRef(S.expo, &e.curve, e.curve));
    j.set("switch", switchRef(e.swtch, tok(S.expo, "swtch", &e)));
    j.set("flight_modes", fmMask(e.flightModes));
    static const char* sides[] = {"none", "positive", "negative", "both"};
    j.set("side", sides[e.mode & 3]);
    arr.push(j);
  }
  return arr;
}

template <class M>
static auto delayPrecOf(const M& m, int) -> decltype(m.delayPrec, int())
{
  return m.delayPrec;
}
template <class M>
static int delayPrecOf(const M&, long)
{
  return 0;
}

J mixes()
{
  J arr = J::arr();
  for (int i = 0; i < MAX_MIXERS; ++i) {
    const MixData& x = g_model.mixData[i];
    if (x.srcRaw == 0) break;
    J j = J::obj();
    j.set("line", i + 1);
    j.set("channel", (long)x.destCh + 1);
    j.set("channel_name", FSTR(g_model.limitData[x.destCh].name));
    j.set("name", FSTR(x.name));
    j.set("source", sourceRef(x.srcRaw, tok(S.mix, "srcRaw", &x)));
    j.set("weight", valueRef(S.mix, "weight", &x, "%"));
    j.set("offset", valueRef(S.mix, "offset", &x, "%"));
    j.set("curve", curveRef(S.mix, &x.curve, x.curve));
    j.set("op", tok(S.mix, "mltpx", &x));
    j.set("switch", switchRef(x.swtch, tok(S.mix, "swtch", &x)));
    j.set("flight_modes", fmMask(x.flightModes));
    j.set("include_trim", x.carryTrim == 0);
    const double sp = x.speedPrec ? 100.0 : 10.0;
    const double dp = delayPrecOf(x, 0) ? 100.0 : 10.0;
    j.set("slow_up_s", J::real(x.speedUp / sp));
    j.set("slow_down_s", J::real(x.speedDown / sp));
    j.set("delay_up_s", J::real(x.delayUp / dp));
    j.set("delay_down_s", J::real(x.delayDown / dp));
    j.set("warning", (long)x.mixWarn);
    arr.push(j);
  }
  return arr;
}

J outputs()
{
  J arr = J::arr();
  bool used[MAX_OUTPUT_CHANNELS] = {false};
  for (int i = 0; i < MAX_MIXERS; ++i) {
    if (g_model.mixData[i].srcRaw == 0) break;
    if (g_model.mixData[i].destCh < MAX_OUTPUT_CHANNELS) used[g_model.mixData[i].destCh] = true;
  }
  for (int i = 0; i < MAX_OUTPUT_CHANNELS; ++i) {
    const LimitData& l = g_model.limitData[i];
    // a name alone doesn't change what the channel does
    LimitData z = l;
    memset(z.name, 0, sizeof(z.name));
    bool nondefault = !yaml_is_zero((uint8_t*)&z, 0, sizeof(z) * 8);
    if (!used[i] && !nondefault) continue;
    J j = J::obj();
    j.set("channel", i + 1);
    j.set("name", FSTR(l.name));
    j.set("min", valueRef(S.limit, "min", &l, "%", -100, 0.1));
    j.set("max", valueRef(S.limit, "max", &l, "%", 100, 0.1));
    j.set("subtrim", valueRef(S.limit, "offset", &l, "%", 0, 0.1));
    j.set("ppm_center_us", (long)(1500 + l.ppmCenter));
    j.set("symmetrical", (bool)l.symetrical);
    j.set("reversed", (bool)l.revert);
    J c = J::obj();
    c.set("value", (long)l.curve);
    c.set("text", l.curve ? uiText(getCurveString(l.curve)) : std::string());
    j.set("curve", c);
    j.set("mixed", used[i]);
    arr.push(j);
  }
  return arr;
}

J logicalSwitches()
{
  J arr = J::arr();
  for (int i = 0; i < MAX_LOGICAL_SWITCHES; ++i) {
    const LogicalSwitchData& l = g_model.logicalSw[i];
    if (l.func == LS_FUNC_NONE) continue;
    J j = J::obj();
    j.set("index", i + 1);
    j.set("func", tok(S.ls, "func", &l));
    j.set("def", tok(S.ls, "def", &l));
    switch (lswFamily(l.func)) {
      case LS_FAMILY_BOOL:
      case LS_FAMILY_STICKY:
        j.set("v1", switchRef(l.v1, ""));
        j.set("v2", switchRef(l.v2, ""));
        break;
      case LS_FAMILY_EDGE:
        j.set("v1", switchRef(l.v1, ""));
        break;
      case LS_FAMILY_COMP:
        j.set("v1", sourceRef(l.v1, ""));
        j.set("v2", sourceRef(l.v2, ""));
        break;
      case LS_FAMILY_TIMER:
        break;
      default:  // LS_FAMILY_OFS: source vs value (raw units of the source)
        j.set("v1", sourceRef(l.v1, ""));
        j.set("v2", (long)l.v2);
        break;
    }
    j.set("and_switch", switchRef(l.andsw, tok(S.ls, "andsw", &l)));
    j.set("delay_s", J::real(l.delay / 10.0));
    j.set("duration_s", J::real(l.duration / 10.0));
    arr.push(j);
  }
  return arr;
}

J specialFunctions()
{
  J arr = J::arr();
  for (int i = 0; i < MAX_SPECIAL_FUNCTIONS; ++i) {
    const CustomFunctionData& f = g_model.customFn[i];
    if (f.isEmpty()) continue;
    J j = J::obj();
    j.set("index", i + 1);
    j.set("switch", switchRef(f.swtch, tok(S.cfn, "swtch", &f)));
    j.set("func", tok(S.cfn, "func", &f));
    j.set("def", tok(S.cfn, "def", &f));
    j.set("enabled", (bool)CFN_ACTIVE(&f));
    arr.push(j);
  }
  return arr;
}

J globalVars()
{
  J arr = J::arr();
  for (int i = 0; i < MAX_GVARS; ++i) {
    const GVarData& g = g_model.gvars[i];
    std::string name = FSTR(g.name);
    J vals = J::arr();
    bool any = !name.empty();
    for (int f = 0; f < MAX_FLIGHT_MODES; ++f) {
      gvar_t v = g_model.flightModeData[f].gvars[i];
      J x = J::obj();
      x.set("fm", f);
      if (f == 0 || v <= GVAR_MAX) {
        x.set("value", J::real(g.prec ? v / 10.0 : (double)v));
        if (v != 0) any = true;
      } else {
        // > GVAR_MAX: use the value of another flight mode (gvars.cpp)
        int o = v - GVAR_MAX - 1;
        if (o >= f) o++;
        if (o == 0) continue;  // default: same as FM0
        x.set("uses_fm", (long)o);
        any = true;
      }
      vals.push(x);
    }
    if (!any) continue;
    J j = J::obj();
    j.set("index", i + 1);
    j.set("name", name);
    j.set("unit", g.unit == 1 ? "%" : "");
    j.set("prec", (long)g.prec);
    j.set("values", vals);
    arr.push(j);
  }
  return arr;
}

J describeModel(const std::string& path, const FileResult& r,
                const std::map<int, MultiProto>& multi, bool haveMulti)
{
  J m = J::obj();
  m.set("file", path);
  m.set("kind", "model");
  m.set("semver", r.semver);
  m.set("loaded", r.loadOk);
  m.set("lint", lintJson(r));
  if (!r.loadOk) return m;

  m.set("name", FSTR(g_model.header.name));
#if ETX_VERSION_NUM >= 21000 && defined(COLORLCD)
  m.set("labels", FSTR(g_model.header.labels));
  m.set("bitmap", FSTR(g_model.header.bitmap));
#endif
  m.set("modules", modules(multi, haveMulti));
  m.set("trainer_mode", tok(S.trainer, "mode", &g_model.trainerData));
  m.set("timers", timers());
  m.set("flight_modes", flightModes());
  m.set("inputs", inputs());
  m.set("mixes", mixes());
  m.set("outputs", outputs());
  m.set("logical_switches", logicalSwitches());
  m.set("special_functions", specialFunctions());
  m.set("global_variables", globalVars());
  m.set("throttle_warning_disabled", (bool)g_model.disableThrottleWarning);
  m.set("switch_warning", tok(S.model, "switchWarningState", &g_model));
  {
    J a = J::obj();
    a.set("disabled", (bool)g_model.disableTelemetryWarning);
    a.set("warning", (long)g_model.rfAlarms.warning);
    a.set("critical", (long)g_model.rfAlarms.critical);
    m.set("rssi_alarms", a);
  }
  J sensors = J::arr();
  for (int i = 0; i < MAX_TELEMETRY_SENSORS; ++i) {
    std::string l = FSTR(g_model.telemetrySensors[i].label);
    if (l.empty()) continue;
    J s = J::obj();
    s.set("index", i + 1);
    s.set("label", l);
    sensors.push(s);
  }
  m.set("telemetry_sensors", sensors);
  m.set("extended_limits", (bool)g_model.extendedLimits);
  m.set("helicopter", g_model.swashR.type != SWASH_TYPE_NONE);
  return m;
}

J describeRadio(const std::string& path, const FileResult& r)
{
  J m = J::obj();
  m.set("file", path);
  m.set("kind", "radio");
  m.set("semver", r.semver);
  m.set("loaded", r.loadOk);
  m.set("lint", lintJson(r));
  if (!r.loadOk) return m;
  m.set("stick_mode", (long)g_eeGeneral.stickMode + 1);
  J sw = J::arr();
  static const char* types[] = {"none", "toggle", "2pos", "3pos"};
  for (int i = 0; i < switchGetMaxSwitches(); ++i) {
#if defined(SWITCH_CONFIG)
    int t = SWITCH_CONFIG(i);
#else
    int t = g_model.getSwitchType(i);
#endif
    char name[32] = {0};
    getSwitchName(name, i);
    J s = J::obj();
    s.set("name", uiText(name));
    s.set("type", t >= 0 && t < 4 ? types[t] : "?");
    sw.push(s);
  }
  m.set("switches", sw);
  J pots = J::arr();
  for (int i = 0; i < adcGetMaxInputs(ADC_INPUT_FLEX); ++i) {
    J p = J::obj();
    p.set("index", i + 1);
    p.set("name", uiText(analogGetCanonicalName(ADC_INPUT_FLEX, i)));
    p.set("label", uiText(getPotLabel(i)));
    pots.push(p);
  }
  m.set("pots", pots);
  return m;
}

bool isRadioFile(const std::string& path)
{
  auto s = path.find_last_of('/');
  std::string b = s == std::string::npos ? path : path.substr(s + 1);
  for (auto& c : b) c = (char)tolower((unsigned char)c);
  return b == "radio.yml";
}

}  // namespace

int describeFiles(FILE* out, const std::vector<std::string>& files, const DescribeOptions& opt)
{
  S.init();
  std::map<int, MultiProto> multi;
  bool haveMulti = !opt.multiFile.empty();
  if (haveMulti) {
    std::ifstream f(opt.multiFile);
    if (!f) {
      fprintf(stderr, "edgetx-lint: cannot read --multi file %s\n", opt.multiFile.c_str());
      return 2;
    }
    multi = loadMulti(opt.multiFile);
  }

  Options lo;
  lo.format = "json";

  J doc = J::obj();
  doc.set("schema", 1);
  J tool = J::obj();
  tool.set("name", "edgetx-lint");
  tool.set("version", LINT_VERSION);
  tool.set("radio", LINT_RADIO);
  tool.set("edgetx", LINT_EDGETX_TAG);
  doc.set("tool", tool);

  int errors = 0, warnings = 0;
  auto count = [&](const FileResult& r) {
    for (auto& f : r.findings) {
      if (f.sev == Severity::Error) errors++;
      else if (f.sev == Severity::Warning) warnings++;
    }
  };

  // radio settings first: switch names/types and pot labels depend on them
  J radio = J::null();
  bool radioSeen = false;
  for (auto& f : files) {
    if (!isRadioFile(f)) continue;
    if (radioSeen) {
      fprintf(stderr, "edgetx-lint: describe takes at most one radio.yml\n");
      return 2;
    }
    radioSeen = true;
    Options ro = lo;
    ro.forceType = "radio";
    FileResult r = lintFile(f, ro);
    count(r);
    radio = describeRadio(f, r);
  }
  if (!radioSeen) generalDefault();
  doc.set("radio", radio);

  J models = J::arr();
  for (auto& f : files) {
    if (isRadioFile(f)) continue;
    Options mo = lo;
    mo.forceType = "model";
    FileResult r = lintFile(f, mo);
    count(r);
    models.push(describeModel(f, r, multi, haveMulti));
  }
  doc.set("models", models);

  std::string o;
  doc.dump(o, 0, opt.pretty);
  o += '\n';
  fwrite(o.data(), 1, o.size(), out);
  return errors || (opt.strict && warnings) ? 1 : 0;
}
