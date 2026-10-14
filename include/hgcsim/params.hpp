// =============================================================================
// HGCSim - 大规模图聚类异构计算模拟器
// params.hpp : 可调参数系统（带默认值 / 上下界 / 单位 / 说明）
//
// 每个硬件组件（计算设备、存储层级、互连链路）都持有一个 ParamSet。
// 所有参数都有：默认值（对应“普通计算机”或典型器件）、下界、上界。
// 上界取自截至 2026 年的公开器件参数（出处见 docs/PARAM_SOURCES.md）。
// 超出上下界的配置会被校验器拒绝；参数之间的关系约束由各组件的
// validate() 实现（例如 HBM4 接口位宽与单栈容量的关系）。
// =============================================================================
#pragma once
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace hgc {

using json = nlohmann::json;

struct ParamSpec {
  std::string name;
  double value = 0;
  double def = 0;
  double min = 0;
  double max = 0;
  std::string unit;
  std::string desc;
  bool integer = false;
  std::vector<double> choices;  // 非空时只允许取这些离散值
};

class ParamSet {
 public:
  ParamSpec& add(const std::string& name, double def, double mn, double mx, const std::string& unit,
                 const std::string& desc, bool integer = false, std::vector<double> choices = {}) {
    ParamSpec s;
    s.name = name;
    s.value = s.def = def;
    s.min = mn;
    s.max = mx;
    s.unit = unit;
    s.desc = desc;
    s.integer = integer;
    s.choices = std::move(choices);
    auto it = idx_.find(name);
    if (it != idx_.end()) {
      specs_[it->second] = s;
      return specs_[it->second];
    }
    idx_[name] = specs_.size();
    specs_.push_back(s);
    return specs_.back();
  }

  bool has(const std::string& n) const { return idx_.count(n) > 0; }

  double get(const std::string& n) const {
    auto it = idx_.find(n);
    if (it == idx_.end()) throw std::runtime_error("unknown parameter: " + n);
    return specs_[it->second].value;
  }
  int geti(const std::string& n) const { return static_cast<int>(std::llround(get(n))); }
  bool getb(const std::string& n) const { return get(n) > 0.5; }

  // 修改默认值（用于设备模板），同时修改当前值
  void set_default(const std::string& n, double v) {
    auto& s = spec(n);
    s.def = v;
    s.value = v;
  }

  ParamSpec& spec(const std::string& n) {
    auto it = idx_.find(n);
    if (it == idx_.end()) throw std::runtime_error("unknown parameter: " + n);
    return specs_[it->second];
  }
  const ParamSpec& spec(const std::string& n) const {
    auto it = idx_.find(n);
    if (it == idx_.end()) throw std::runtime_error("unknown parameter: " + n);
    return specs_[it->second];
  }

  // 设置参数并做范围检查；返回空字符串表示成功，否则返回错误信息
  std::string set(const std::string& n, double v) {
    auto it = idx_.find(n);
    if (it == idx_.end()) return "未知参数 '" + n + "'";
    ParamSpec& s = specs_[it->second];
    if (!std::isfinite(v)) return "参数 '" + n + "' 不是有限数值";
    if (s.integer && std::fabs(v - std::round(v)) > 1e-9)
      return "参数 '" + n + "' 必须为整数";
    if (!s.choices.empty()) {
      bool ok = false;
      for (double c : s.choices)
        if (std::fabs(c - v) < 1e-9) ok = true;
      if (!ok) {
        std::string msg = "参数 '" + n + "'=" + fmt(v) + " 不在允许取值 {";
        for (size_t i = 0; i < s.choices.size(); ++i) msg += (i ? "," : "") + fmt(s.choices[i]);
        return msg + "} 内";
      }
    }
    if (v < s.min - 1e-12 || v > s.max + 1e-12)
      return "参数 '" + n + "'=" + fmt(v) + " 超出范围 [" + fmt(s.min) + ", " + fmt(s.max) + "] " + s.unit;
    s.value = v;
    return "";
  }

  // 从 JSON 对象读入参数，错误追加到 errors（带上下文前缀）
  void load(const json& j, std::vector<std::string>& errors, const std::string& ctx) {
    if (j.is_null()) return;
    if (!j.is_object()) {
      errors.push_back(ctx + ": params 必须是对象");
      return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      double v;
      if (it.value().is_boolean())
        v = it.value().get<bool>() ? 1.0 : 0.0;
      else if (it.value().is_number())
        v = it.value().get<double>();
      else {
        errors.push_back(ctx + ": 参数 '" + it.key() + "' 必须是数值");
        continue;
      }
      std::string e = set(it.key(), v);
      if (!e.empty()) errors.push_back(ctx + ": " + e);
    }
  }

  json describe() const {
    json arr = json::array();
    for (auto& s : specs_) {
      json o = {{"name", s.name}, {"value", s.value}, {"default", s.def}, {"min", s.min},
                {"max", s.max},   {"unit", s.unit},   {"desc", s.desc},   {"integer", s.integer}};
      if (!s.choices.empty()) o["choices"] = s.choices;
      arr.push_back(o);
    }
    return arr;
  }

  json values() const {
    json o = json::object();
    for (auto& s : specs_) o[s.name] = s.value;
    return o;
  }

  const std::vector<ParamSpec>& all() const { return specs_; }

  static std::string fmt(double v) {
    char buf[64];
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15)
      snprintf(buf, sizeof buf, "%.0f", v);
    else
      snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
  }

 private:
  std::vector<ParamSpec> specs_;
  std::map<std::string, size_t> idx_;
};

// 常用单位换算
constexpr double KB = 1024.0;
constexpr double MB = 1024.0 * 1024.0;
constexpr double GB = 1024.0 * 1024.0 * 1024.0;
constexpr double TB = 1024.0 * GB;
constexpr double GBps = 1e9;  // 带宽 GB/s 采用十进制
constexpr double NS = 1e-9;
constexpr double US = 1e-6;
constexpr double MS = 1e-3;

}  // namespace hgc
