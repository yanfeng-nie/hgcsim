#include "hgcsim/system.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace hgc {

namespace {

struct MemTemplate {
  std::string kind;
  json params;
};

// 各类设备在未给出 memory 配置时的默认本地存储模板
MemTemplate default_memory_for(const std::string& type) {
  if (type == "GPU") return {"GDDR", json::object()};
  if (type == "NPU") return {"HBM", {{"stacks", 4}, {"gbps_per_pin", 3.2}, {"gb_per_stack", 16}}};
  if (type == "TPU") return {"HBM", {{"stacks", 2}, {"gbps_per_pin", 3.2}, {"gb_per_stack", 8}}};
  if (type == "FPGA") return {"HBM", {{"stacks", 2}, {"gbps_per_pin", 3.2}, {"gb_per_stack", 16}}};
  if (type == "NMP") return {"DRAM", {{"channels", 10}, {"data_rate_mtps", 2400}}};
  if (type == "CSD")
    return {"FLASH",
            {{"nand_channels", 16}, {"channel_mtps", 1200}, {"dies", 128}, {"read_latency_us", 60},
             {"controller_bw_gbps", 16}, {"capacity_tb", 4}, {"controller_iops_m", 1.0}}};
  if (type == "CIM_RRAM" || type == "CIM_MRAM")
    return {"DRAM", {{"channels", 4}, {"channel_width_bits", 32}, {"data_rate_mtps", 6400}, {"capacity_gb", 16}}};
  if (type == "SSD") return {"FLASH", json::object()};
  if (type == "MEM_EXPANDER") return {"DRAM", {{"channels", 2}, {"capacity_gb", 128}}};
  return {"DRAM", json::object()};
}

std::string mem_suffix(MemKind k) { return k == MemKind::FLASH ? ".flash" : ".mem"; }

std::unique_ptr<Switch> make_switch(const std::string& id, const std::string& type) {
  auto s = std::make_unique<Switch>();
  s->id = id;
  s->type = type;
  if (type == "CXL") {
    s->p.add("latency_ns", 120, 50, 500, "ns", "交换机转发延迟");
    s->p.add("max_version", 3.0, 2.0, 4.0, "", "交换机支持的最高 CXL 版本", false, {2.0, 3.0, 3.1, 3.2, 4.0});
    s->p.add("ports", 16, 2, 64, "个", "下游端口数", true);
  } else {
    s->p.add("latency_ns", 150, 50, 500, "ns", "交换机转发延迟");
    s->p.add("max_gen", 5, 3, 7, "", "交换机支持的最高 PCIe 代数", true);
    s->p.add("p2p_enabled", 1, 0, 1, "", "是否允许下游设备间 P2P 直传", true);
    s->p.add("ports", 8, 2, 64, "个", "下游端口数", true);
  }
  return s;
}

}  // namespace

ComputeDevice* System::device(const std::string& id) const {
  for (auto& d : devices)
    if (d->id == id) return d.get();
  return nullptr;
}
MemoryTier* System::memory(const std::string& id) const {
  for (auto& m : memories)
    if (m->id == id) return m.get();
  return nullptr;
}
Switch* System::sw(const std::string& id) const {
  for (auto& s : switches)
    if (s->id == id) return s.get();
  return nullptr;
}
bool System::is_node(const std::string& id) const { return node_kind_.count(id) > 0; }

std::unique_ptr<System> System::from_json(const json& j, std::vector<std::string>& errors) {
  auto sys = std::make_unique<System>();
  sys->source = j;
  sys->name = j.value("name", "unnamed");
  sys->description = j.value("description", "");
  std::set<std::string> ids;
  auto claim = [&](const std::string& id) {
    if (id.empty()) {
      errors.push_back("存在空 id");
      return false;
    }
    if (!ids.insert(id).second) {
      errors.push_back("重复的 id '" + id + "'");
      return false;
    }
    return true;
  };

  // 工具：创建存储
  auto build_memory = [&](const json& mj, ComputeDevice* dev) -> MemoryTier* {
    MemTemplate t = default_memory_for(dev->type);
    std::string kind = t.kind;
    if (mj.is_object() && mj.contains("type")) kind = mj["type"].get<std::string>();
    MemKind mk;
    if (!parse_mem_kind(kind, mk)) {
      errors.push_back("设备 '" + dev->id + "': 未知存储类型 '" + kind + "'");
      return nullptr;
    }
    auto allowed = dev->allowed_memory_kinds();
    if (std::find(allowed.begin(), allowed.end(), kind) == allowed.end()) {
      std::string al;
      for (auto& a : allowed) al += a + " ";
      errors.push_back("设备 '" + dev->id + "' (" + dev->type + ") 不能使用 " + kind + " 作为本地存储，允许: " + al);
      return nullptr;
    }
    std::string mid = (mj.is_object() && mj.contains("id")) ? mj["id"].get<std::string>() : dev->id + mem_suffix(mk);
    if (!claim(mid)) return nullptr;
    auto m = make_memory(mid, mk);
    m->owner = dev->id;
    if (kind == t.kind) m->p.load(t.params, errors, "存储 '" + mid + "'(模板)");
    json up = (mj.is_object() && mj.contains("params")) ? mj["params"] : json::object();
    if (dev->type == "NMP" && !up.contains("capacity_gb") && mk == MemKind::DRAM) {
      double cap = dev->p.get("units") * dev->p.get("mb_per_unit") / 1024.0;
      m->p.set("capacity_gb", std::max(1.0, cap));
    }
    if (dev->type == "NMP" && mk == MemKind::HBM && !up.contains("gb_per_stack")) {
      double cap = dev->p.get("units") * dev->p.get("mb_per_unit") / 1024.0;
      m->p.set("gb_per_stack", std::min(64.0, std::max(1.0, cap / m->p.get("stacks"))));
    }
    m->p.load(up, errors, "存储 '" + mid + "'");
    m->validate(errors);
    MemoryTier* raw = m.get();
    sys->memories.push_back(std::move(m));
    sys->node_kind_[mid] = "memory";
    return raw;
  };

  auto add_link = [&](const std::string& a, const std::string& b, const json& lj, const std::string& ctx) -> Link* {
    std::string lt = lj.value("type", "PCIe");
    LinkKind lk;
    if (!parse_link_kind(lt, lk)) {
      errors.push_back(ctx + ": 未知链路类型 '" + lt + "'");
      return nullptr;
    }
    auto l = std::make_unique<Link>(a + "<->" + b, lk);
    l->a = a;
    l->b = b;
    l->p.load(lj.value("params", json::object()), errors, ctx + " 链路");
    l->validate(errors);
    Link* raw = l.get();
    sys->links.push_back(std::move(l));
    return raw;
  };

  // ---- 主机 ----
  json hj = j.value("host", json::object());
  std::string hid = hj.value("id", "cpu0");
  claim(hid);
  auto host = std::make_unique<CPUDevice>(hid);
  host->p.load(hj.value("params", json::object()), errors, "主机 '" + hid + "'");
  ComputeDevice* hostp = host.get();
  sys->devices.push_back(std::move(host));
  sys->node_kind_[hid] = "device";
  json hm = hj.value("memory", json::object());
  if (!hm.contains("id")) hm["id"] = "dram0";
  hostp->mem = build_memory(hm, hostp);

  // ---- 交换机 ----
  for (auto& sj : j.value("switches", json::array())) {
    std::string sid = sj.value("id", "");
    std::string st = sj.value("type", "PCIe");
    if (st != "PCIe" && st != "CXL") {
      errors.push_back("交换机 '" + sid + "': 类型必须是 PCIe 或 CXL");
      continue;
    }
    if (!claim(sid)) continue;
    auto s = make_switch(sid, st);
    s->p.load(sj.value("params", json::object()), errors, "交换机 '" + sid + "'");
    sys->node_kind_[sid] = "switch";
    sys->switches.push_back(std::move(s));
  }

  // ---- 设备 ----
  for (auto& dj : j.value("devices", json::array())) {
    std::string did = dj.value("id", "");
    std::string dt = dj.value("type", "");
    if (!claim(did)) continue;
    std::string err;
    auto d = make_device(did, dt, &err);
    if (!d) {
      errors.push_back("设备 '" + did + "': " + err);
      continue;
    }
    if (dt == "CPU") {
      errors.push_back("设备 '" + did + "': 本模拟器为单主机系统，CPU 请在 host 中配置");
      continue;
    }
    d->p.load(dj.value("params", json::object()), errors, "设备 '" + did + "'");
    ComputeDevice* dp = d.get();
    sys->devices.push_back(std::move(d));
    sys->node_kind_[did] = "device";
    dp->mem = build_memory(dj.value("memory", json::object()), dp);
  }

  // ---- 挂载链路（交换机先于设备处理，以便设备挂到交换机下）----
  auto attach = [&](const std::string& id, const json& aj, bool is_switch) {
    std::string ctx = (is_switch ? "交换机 '" : "设备 '") + id + "'";
    if (!aj.is_object()) {
      errors.push_back(ctx + ": 缺少 attach 配置");
      return;
    }
    std::string to = aj.value("to", hid);
    if (!sys->node_kind_.count(to) || sys->node_kind_[to] == "memory") {
      errors.push_back(ctx + ": 挂载目标 '" + to + "' 不存在");
      return;
    }
    if (sys->node_kind_[to] == "device" && to != hid) {
      errors.push_back(ctx + ": 只能挂载到主机或交换机上（对等互连请使用 peer_links）");
      return;
    }
    Link* l = add_link(id, to, aj.value("link", json::object()), ctx);
    if (!l) return;
    // ---- 接口能力约束 ----
    Switch* up = sys->sw(to);
    ComputeDevice* dev = sys->device(id);
    Switch* me = sys->sw(id);
    if (l->kind == LinkKind::PCIE) {
      int g = l->p.geti("gen");
      if (dev && g > dev->max_link_gen())
        errors.push_back(ctx + ": 链路 PCIe Gen" + std::to_string(g) + " 超过设备支持的 Gen" + std::to_string(dev->max_link_gen()));
      if (dev && l->p.geti("lanes") > dev->max_lanes())
        errors.push_back(ctx + ": 链路通道数 x" + std::to_string(l->p.geti("lanes")) + " 超过设备支持的 x" + std::to_string(dev->max_lanes()));
      if (!up && g > hostp->max_link_gen())
        errors.push_back(ctx + ": 主机根复合体只支持到 PCIe Gen" + std::to_string(hostp->max_link_gen()));
      if (up && up->type != "PCIe") errors.push_back(ctx + ": CXL 交换机下游只能使用 CXL 链路");
      if (up && g > up->p.geti("max_gen")) errors.push_back(ctx + ": 超过 PCIe 交换机支持的代数");
      if (me && me->type != "PCIe") errors.push_back(ctx + ": CXL 交换机的上游链路必须是 CXL");
    } else if (l->kind == LinkKind::CXL) {
      double v = l->p.get("version");
      if (dev && dev->cxl_max_version() + 1e-9 < v)
        errors.push_back(ctx + ": 设备不支持 CXL " + ParamSet::fmt(v) + "（设备 cxl_max_version=" +
                         ParamSet::fmt(dev->cxl_max_version()) + "）");
      if (dev && l->p.geti("lanes") > dev->max_lanes()) errors.push_back(ctx + ": 链路通道数超过设备支持");
      if (!up && hostp->cxl_max_version() + 1e-9 < v)
        errors.push_back(ctx + ": 主机 CPU 不支持 CXL " + ParamSet::fmt(v) + "（host.cxl_max_version=" +
                         ParamSet::fmt(hostp->cxl_max_version()) + "）");
      if (up) {
        if (up->type != "CXL") errors.push_back(ctx + ": PCIe 交换机下游不能使用 CXL 链路");
        if (v < 2.0) errors.push_back(ctx + ": 经过 CXL 交换机需要 CXL ≥ 2.0");
        if (v > up->p.get("max_version") + 1e-9) errors.push_back(ctx + ": 超过 CXL 交换机支持的版本");
      }
      if (me) {
        if (me->type != "CXL") errors.push_back(ctx + ": PCIe 交换机上游不能使用 CXL 链路");
        if (v < 2.0) errors.push_back(ctx + ": CXL 交换需要 CXL ≥ 2.0");
        if (up && v < 3.0) errors.push_back(ctx + ": 多级 CXL 交换（交换机级联）需要 CXL ≥ 3.0");
      }
      if (dev && dev->type == "SSD") errors.push_back(ctx + ": 普通 SSD 不支持 CXL，请使用 PCIe 或 MEM_EXPANDER");
    } else if (l->kind == LinkKind::DDR) {
      if (!dev || (dev->type != "NMP" && !(dev->type == "MEM_EXPANDER" && dev->mem && dev->mem->kind == MemKind::SCM)))
        errors.push_back(ctx + ": DDR 总线只能挂载 NMP（PIM DIMM）或 SCM 形态的 MEM_EXPANDER（持久内存 DIMM）");
      if (to != hid) errors.push_back(ctx + ": DDR 总线设备只能直接挂在主机上");
    } else if (l->kind == LinkKind::NVLINK) {
      errors.push_back(ctx + ": NVLink 只能用于 peer_links");
    }
    if (dev && dev->type == "MEM_EXPANDER" && l->kind == LinkKind::PCIE)
      errors.push_back(ctx + ": 内存扩展器必须通过 CXL（或 DDR）挂载");
    if (dev) dev->attach_bw = l->bw();
  };
  for (auto& sj : j.value("switches", json::array())) {
    std::string sid = sj.value("id", "");
    if (sys->sw(sid)) attach(sid, sj.value("attach", json()), true);
  }
  for (auto& dj : j.value("devices", json::array())) {
    std::string did = dj.value("id", "");
    if (sys->device(did) && did != hid) attach(did, dj.value("attach", json()), false);
  }
  // ---- 对等链路 ----
  for (auto& pj : j.value("peer_links", json::array())) {
    std::string a = pj.value("a", ""), b = pj.value("b", "");
    ComputeDevice* da = sys->device(a);
    ComputeDevice* db = sys->device(b);
    if (!da || !db) {
      errors.push_back("peer_link: 设备 '" + a + "' 或 '" + b + "' 不存在");
      continue;
    }
    Link* l = add_link(a, b, pj.value("link", json{{"type", "NVLink"}}), "peer_link " + a + "-" + b);
    if (l && l->kind != LinkKind::NVLINK) errors.push_back("peer_link " + a + "-" + b + ": 只支持 NVLink 类型");
    if (l && !((da->type == "GPU" || da->type == "CPU") && (db->type == "GPU" || db->type == "CPU")))
      errors.push_back("peer_link " + a + "-" + b + ": NVLink 只能连接 GPU-GPU 或 GPU-CPU");
  }
  // ---- 介质链路 ----
  for (auto& m : sys->memories) {
    auto l = std::make_unique<Link>(m->id + ".media", LinkKind::MEDIA);
    l->a = m->id;
    l->b = m->owner;
    l->media = m.get();
    sys->links.push_back(std::move(l));
  }
  // ---- 全局关系约束 ----
  {
    double ddr_ch = 0;
    for (auto& l : sys->links)
      if (l->kind == LinkKind::DDR) ddr_ch += l->p.get("channels");
    if (ddr_ch > 0 && hostp->mem) {
      double host_ch = hostp->mem->p.get("channels");
      if (ddr_ch + host_ch > 24)
        errors.push_back("DDR 总线设备占用 " + ParamSet::fmt(ddr_ch) + " 个通道 + 主机 DRAM " + ParamSet::fmt(host_ch) +
                         " 个通道，超过平台最多 24 个内存通道");
    }
    std::map<std::string, int> fanout;
    for (auto& l : sys->links)
      if (sys->sw(l->b)) fanout[l->b]++;
    for (auto& kv : fanout) {
      Switch* s = sys->sw(kv.first);
      if (kv.second > s->p.geti("ports"))
        errors.push_back("交换机 '" + kv.first + "' 下游设备数 " + std::to_string(kv.second) + " 超过端口数");
    }
  }
  for (auto& d : sys->devices) {
    if (!d->mem && d->needs_local_memory()) errors.push_back("设备 '" + d->id + "' 缺少本地存储");
    d->validate(errors);
  }
  json bj = j.value("budget", json::object());
  sys->budget_enabled = bj.value("enabled", false);
  sys->power_budget_w = bj.value("power_w", 0.0);
  sys->cost_budget_usd = bj.value("cost_usd", 0.0);
  if (sys->budget_enabled) {
    if (sys->power_budget_w > 0 && sys->total_tdp() > sys->power_budget_w)
      errors.push_back("系统总功耗 " + ParamSet::fmt(std::round(sys->total_tdp())) + " W 超出预算 " +
                       ParamSet::fmt(sys->power_budget_w) + " W");
    if (sys->cost_budget_usd > 0 && sys->total_cost() > sys->cost_budget_usd)
      errors.push_back("系统总成本 $" + ParamSet::fmt(std::round(sys->total_cost())) + " 超出预算 $" +
                       ParamSet::fmt(sys->cost_budget_usd));
  }
  sys->build_index();
  return sys;
}

void System::build_index() {
  adj_.clear();
  for (size_t i = 0; i < links.size(); ++i) {
    adj_[links[i]->a].push_back(static_cast<int>(i));
    adj_[links[i]->b].push_back(static_cast<int>(i));
  }
}

double System::node_latency(const std::string& n) const {
  if (Switch* s = sw(n)) return s->latency_s();
  if (ComputeDevice* d = device(n)) {
    if (auto* sd = dynamic_cast<StorageDevice*>(d)) return sd->controller_latency_s();
  }
  return 0;
}

Path System::compute_route(const std::string& src, const std::string& dst) const {
  Path best;
  best.error = "无法从 '" + src + "' 路由到 '" + dst + "'";
  if (!is_node(src) || !is_node(dst)) {
    best.error = "未知节点 '" + (is_node(src) ? dst : src) + "'";
    return best;
  }
  if (src == dst) {
    best.ok = true;
    best.bottleneck_bw = 1e18;
    best.nodes = {src};
    return best;
  }
  const std::string hid = host()->id;
  auto mem_owner = [&](const std::string& n) -> std::string {
    MemoryTier* m = memory(n);
    return m ? m->owner : "";
  };
  std::string src_owner = mem_owner(src), dst_owner = mem_owner(dst);
  std::vector<int> cur_links, cur_dirs;
  std::vector<std::string> cur_nodes{src};
  std::set<std::string> visited{src};

  std::function<void(const std::string&, int)> dfs = [&](const std::string& n, int in_link) {
    if (n == dst) {
      Path p;
      p.ok = true;
      p.links = cur_links;
      p.dirs = cur_dirs;
      p.nodes = cur_nodes;
      p.bottleneck_bw = 1e18;
      for (size_t i = 0; i < cur_links.size(); ++i) {
        const Link* l = links[cur_links[i]].get();
        p.bottleneck_bw = std::min(p.bottleneck_bw, l->bw());
        p.latency_s += l->latency_s();
        p.setup_s = std::max(p.setup_s, l->setup_s());
        p.load_store = p.load_store && l->load_store();
        p.zero_copy_eff = std::min(p.zero_copy_eff, l->zero_copy_efficiency());
        if (l->kind == LinkKind::MEDIA) {
          if (i == 0) p.has_media_src = true;
          if (i + 1 == cur_links.size()) p.has_media_dst = true;
        } else {
          p.pj_per_bit += l->pj_per_bit();
        }
      }
      for (size_t i = 1; i + 1 < cur_nodes.size(); ++i) p.latency_s += node_latency(cur_nodes[i]);
      bool better = !best.ok || p.bottleneck_bw > best.bottleneck_bw * (1 + 1e-9) ||
                    (std::fabs(p.bottleneck_bw - best.bottleneck_bw) <= best.bottleneck_bw * 1e-9 &&
                     p.latency_s < best.latency_s);
      if (better) best = p;
      return;
    }
    // 中间节点规则
    if (n != src) {
      std::string kind = node_kind_.at(n);
      if (kind == "memory") return;
      if (kind == "device" && n != hid) {
        bool owner_hop = (n == src_owner) || (n == dst_owner);
        if (!owner_hop && n != src) {
          // 允许经 NVLink 对等链路进入/离开（只做一次转发）
          bool via_peer = in_link >= 0 && links[in_link]->kind == LinkKind::NVLINK;
          if (!via_peer) return;
        }
      }
    }
    for (int li : adj_.at(n)) {
      if (li == in_link) continue;
      const Link* l = links[li].get();
      std::string nxt = (l->a == n) ? l->b : l->a;
      if (visited.count(nxt)) continue;
      // 交换机处的 P2P 转向约束：入、出链路都是该交换机的下游链路
      if (Switch* s = sw(n)) {
        if (in_link >= 0 && links[in_link]->b == n && l->b == n) {
          bool ok;
          if (s->type == "PCIe")
            ok = s->p.getb("p2p_enabled");
          else
            ok = links[in_link]->cxl_version() >= 3.0 && l->cxl_version() >= 3.0 && s->p.get("max_version") >= 3.0;
          if (!ok) continue;
        }
      }
      visited.insert(nxt);
      cur_links.push_back(li);
      cur_dirs.push_back(l->a == n ? 0 : 1);
      cur_nodes.push_back(nxt);
      dfs(nxt, li);
      cur_nodes.pop_back();
      cur_dirs.pop_back();
      cur_links.pop_back();
      visited.erase(nxt);
    }
  };
  dfs(src, -1);
  if (!best.ok && src != hid && dst != hid) {
    // 无法直接 P2P（交换机不允许下游互访）：经主机根复合体“发夹”转发
    Path a = compute_route(src, hid);
    Path b = compute_route(hid, dst);
    if (a.ok && b.ok) {
      Path p = a;
      p.links.insert(p.links.end(), b.links.begin(), b.links.end());
      p.dirs.insert(p.dirs.end(), b.dirs.begin(), b.dirs.end());
      p.nodes.insert(p.nodes.end(), b.nodes.begin() + 1, b.nodes.end());
      p.bottleneck_bw = std::min(a.bottleneck_bw, b.bottleneck_bw);
      p.latency_s = a.latency_s + b.latency_s;
      p.setup_s = std::max(a.setup_s, b.setup_s);
      p.load_store = a.load_store && b.load_store;
      p.zero_copy_eff = std::min(a.zero_copy_eff, b.zero_copy_eff);
      p.pj_per_bit = a.pj_per_bit + b.pj_per_bit;
      p.has_media_dst = b.has_media_dst;
      best = p;
    }
  }
  return best;
}

const Path& System::route(const std::string& src, const std::string& dst) const {
  auto key = std::make_pair(src, dst);
  auto it = route_cache_.find(key);
  if (it != route_cache_.end()) return it->second;
  return route_cache_[key] = compute_route(src, dst);
}

double System::total_cost() const {
  double c = 0;
  for (auto& d : devices) c += d->cost_usd();
  for (auto& m : memories) c += m->cost_usd();
  for (auto& s : switches) c += s->type == "CXL" ? 800 : 300;
  return c;
}
double System::total_tdp() const {
  double w = 0;
  for (auto& d : devices) w += d->tdp_w();
  for (auto& m : memories) w += m->peak_power_w();
  w += 25.0 * switches.size();
  return w;
}
double System::total_idle_w() const {
  double w = 0;
  for (auto& d : devices) w += d->idle_w();
  for (auto& m : memories) w += m->static_power_w();
  w += 10.0 * switches.size();
  return w;
}

json System::describe() const {
  json o;
  o["name"] = name;
  o["description"] = description;
  json devs = json::array();
  for (auto& d : devices) {
    json dj = d->describe_derived();
    dj["params"] = d->p.values();
    dj["has_compute"] = d->has_compute();
    devs.push_back(dj);
  }
  o["devices"] = devs;
  json mems = json::array();
  for (auto& m : memories) mems.push_back(m->describe_derived());
  o["memories"] = mems;
  json ls = json::array();
  for (auto& l : links) ls.push_back(l->describe_derived());
  o["links"] = ls;
  json ss = json::array();
  for (auto& s : switches) ss.push_back({{"id", s->id}, {"type", s->type}, {"params", s->p.values()}});
  o["switches"] = ss;
  json nodes = json::array();
  for (auto& kv : node_kind_) {
    json nj = {{"id", kv.first}, {"kind", kv.second}};
    if (auto* d = device(kv.first)) nj["type"] = d->type;
    if (auto* m = memory(kv.first)) nj["type"] = mem_kind_name(m->kind);
    if (auto* s = sw(kv.first)) nj["type"] = s->type + "-Switch";
    nodes.push_back(nj);
  }
  o["topology"] = {{"nodes", nodes}};
  o["totals"] = {{"cost_usd", total_cost()},
                 {"tdp_w", total_tdp()},
                 {"idle_w", total_idle_w()},
                 {"budget_enabled", budget_enabled},
                 {"power_budget_w", power_budget_w},
                 {"cost_budget_usd", cost_budget_usd}};
  return o;
}

void System::reset_stats() {
  for (auto& d : devices) d->reset_stats();
  for (auto& m : memories) m->reset_stats();
  for (auto& l : links) l->reset_stats();
}

json catalog() {
  json o;
  json devs = json::object();
  for (auto& t : device_types()) {
    auto d = make_device("x", t, nullptr);
    MemTemplate mt = default_memory_for(t);
    json allowed = d->allowed_memory_kinds();
    devs[t] = {{"params", d->p.describe()},
               {"default_memory", {{"type", mt.kind}, {"params", mt.params}}},
               {"allowed_memory", allowed},
               {"has_compute", d->has_compute()}};
  }
  o["devices"] = devs;
  json mems = json::object();
  for (auto k : {MemKind::HBM, MemKind::GDDR, MemKind::DRAM, MemKind::SCM, MemKind::FLASH}) {
    MemoryTier m("x", k);
    mems[mem_kind_name(k)] = m.p.describe();
  }
  o["memories"] = mems;
  json ls = json::object();
  for (auto k : {LinkKind::PCIE, LinkKind::CXL, LinkKind::DDR, LinkKind::NVLINK}) {
    Link l("x", k);
    ls[link_kind_name(k)] = l.p.describe();
  }
  o["links"] = ls;
  o["switches"] = {{"PCIe", make_switch("x", "PCIe")->p.describe()}, {"CXL", make_switch("x", "CXL")->p.describe()}};
  return o;
}

}  // namespace hgc
