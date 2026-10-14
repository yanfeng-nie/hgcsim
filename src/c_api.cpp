#include "hgcsim/c_api.h"

#include "hgcsim_build_info.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "hgcsim/runtime.hpp"
#include "hgcsim/system.hpp"

using hgc::json;

namespace {
char* dup(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}
char* wrap(const std::function<json(const json&)>& fn, const char* in) {
  try {
    json j = json::parse(in ? in : "{}");
    return dup(fn(j).dump(-1, ' ', false, json::error_handler_t::replace));
  } catch (std::exception& ex) {
    json e = {{"status", "error"}, {"ok", false}, {"errors", {std::string("JSON 解析/执行错误: ") + ex.what()}}};
    return dup(e.dump());
  }
}
}  // namespace

extern "C" {

const char* hgc_version(void) { return "HGCSim 1.1.0"; }

char* hgc_build_info(void) {
  auto csv = [](const std::string& s) {
    json a = json::array();
    std::string cur;
    for (char c : s) {
      if (c == ',') {
        if (!cur.empty()) a.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) a.push_back(cur);
    return a;
  };
  json j = {{"name", HGCSIM_BUILD_NAME},
            {"version", hgc_version()},
            {"time", HGCSIM_BUILD_TIME},
            {"modified", std::string(HGCSIM_BUILD_MODIFIED) == "ON"},
            {"source_fingerprint", HGCSIM_SOURCE_FINGERPRINT},
            {"student_fingerprint", HGCSIM_STUDENT_FINGERPRINT},
            {"plugins", csv(HGCSIM_PLUGINS)},
            {"reserved_names", csv(HGCSIM_RESERVED_NAMES)}};
  return dup(j.dump());
}

char* hgc_catalog(void) { return dup(hgc::catalog().dump()); }

char* hgc_validate_system(const char* s) { return wrap(hgc::validate_system, s); }

char* hgc_characterize(const char* s) { return wrap(hgc::characterize, s); }

char* hgc_plan(const char* s) { return wrap(hgc::plan_strategy, s); }

char* hgc_simulate(const char* s) { return wrap(hgc::simulate, s); }

void hgc_free(char* p) { std::free(p); }
}
