#include "hgcsim/runtime.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <tuple>

#include "hgcsim/scheduler.hpp"
#include "hgcsim/system.hpp"
#include "hgcsim/workload.hpp"

namespace hgc {

namespace {

using Clock = std::chrono::steady_clock;

struct Copy {
  bool home = false;
  bool valid = false;
  int pins = 0;
  double last_use = 0;
};

struct AggKey {
  std::string dev, stage, name;
  bool operator<(const AggKey& o) const {
    return std::tie(dev, stage, name) < std::tie(o.dev, o.stage, o.name);
  }
};
struct Agg {
  double flops = 0, bytes = 0, time = 0;
  long count = 0;
  std::string op, prec;
};

class Sim {
 public:
  Sim(System& sys, const Workload& w, const Strategy& s, TaskGraph& g, IScheduler* plugin, const json& opts)
      : sys_(sys), w_(w), s_(s), g_(g), plugin_(plugin) {
    max_timeline_ = opts.value("max_timeline", 20000);
    wall_limit_ = opts.value("wall_limit_s", 120.0);
  }
  json run();

 private:
  System& sys_;
  const Workload& w_;
  const Strategy& s_;
  TaskGraph& g_;
  IScheduler* plugin_;
  size_t max_timeline_;
  double wall_limit_;
  std::string fatal_;

  // ------------------------------------------------------------- events
  struct Ev {
    double t;
    long seq;
    std::function<void()> fn;
  };
  struct EvCmp {
    bool operator()(const Ev& a, const Ev& b) const { return a.t > b.t || (a.t == b.t && a.seq > b.seq); }
  };
  std::priority_queue<Ev, std::vector<Ev>, EvCmp> pq_;
  double now_ = 0;
  long seq_ = 0, nevents_ = 0;
  void at(double t, std::function<void()> fn) { pq_.push({std::max(t, now_), seq_++, std::move(fn)}); }
  void fail(const std::string& m) {
    if (fatal_.empty()) fatal_ = m;
  }

  // ------------------------------------------------------------- flows
  struct Flow {
    std::vector<int> res;
    double bytes = 0, remaining = 0, cap = 0, rate = 0;
    std::function<void()> done;
    const Path* path = nullptr;
  };
  std::vector<double> res_cap_;
  std::map<int, Flow> flows_;
  int next_flow_ = 0;
  long fver_ = 0;
  double flast_ = 0;

  int resource(int li, int dir) const {
    const Link* l = sys_.links[li].get();
    return l->full_duplex() ? 2 * li + dir : 2 * li;
  }
  void fsettle() {
    double dt = now_ - flast_;
    if (dt > 0)
      for (auto& kv : flows_) kv.second.remaining -= kv.second.rate * dt;
    flast_ = now_;
  }
  void freallocate() {
    std::vector<double> left = res_cap_;
    std::vector<int> cnt(res_cap_.size(), 0);
    std::vector<Flow*> un;
    for (auto& kv : flows_) {
      kv.second.rate = 0;
      un.push_back(&kv.second);
      for (int r : kv.second.res) cnt[r]++;
    }
    int guard = 0;
    while (!un.empty() && guard++ < 10000) {
      double delta = 1e30;
      for (size_t r = 0; r < cnt.size(); ++r)
        if (cnt[r] > 0) delta = std::min(delta, left[r] / cnt[r]);
      for (Flow* f : un) delta = std::min(delta, f->cap - f->rate);
      delta = std::max(delta, 0.0);
      for (Flow* f : un) {
        f->rate += delta;
        for (int r : f->res) left[r] -= delta;
      }
      std::vector<Flow*> keep;
      for (Flow* f : un) {
        bool frozen = f->rate >= f->cap * (1 - 1e-9);
        for (int r : f->res)
          if (left[r] <= res_cap_[r] * 1e-9) frozen = true;
        if (frozen) {
          for (int r : f->res) cnt[r]--;
        } else {
          keep.push_back(f);
        }
      }
      un.swap(keep);
    }
  }
  void fschedule() {
    ++fver_;
    double tmin = 1e300;
    for (auto& kv : flows_) {
      const Flow& f = kv.second;
      if (f.rate > 0) tmin = std::min(tmin, std::max(f.remaining, 0.0) / f.rate);
    }
    if (tmin < 1e299) {
      long v = fver_;
      at(now_ + tmin, [this, v]() { on_flow_event(v); });
    }
  }
  void on_flow_event(long v) {
    if (v != fver_) return;
    fsettle();
    std::vector<std::function<void()>> cbs;
    for (auto it = flows_.begin(); it != flows_.end();) {
      Flow& f = it->second;
      if (f.remaining <= std::max(1.0, f.bytes * 1e-9)) {
        account_flow(f);
        cbs.push_back(std::move(f.done));
        it = flows_.erase(it);
      } else {
        ++it;
      }
    }
    freallocate();
    fschedule();
    for (auto& cb : cbs)
      if (cb) cb();
  }
  void account_flow(const Flow& f) {
    if (!f.path) return;
    for (size_t i = 0; i < f.path->links.size(); ++i) {
      Link* l = sys_.links[f.path->links[i]].get();
      if (f.path->dirs[i] == 0)
        l->bytes_fwd += f.bytes;
      else
        l->bytes_rev += f.bytes;
    }
  }
  void add_flow(const Path* p, double bytes, double cap, std::function<void()> done) {
    if (bytes <= 0) {
      if (done) at(now_, std::move(done));
      return;
    }
    fsettle();
    Flow f;
    f.path = p;
    f.bytes = f.remaining = bytes;
    f.cap = std::max(cap, 1.0);
    f.done = std::move(done);
    for (size_t i = 0; i < p->links.size(); ++i) f.res.push_back(resource(p->links[i], p->dirs[i]));
    flows_[next_flow_++] = std::move(f);
    freallocate();
    fschedule();
  }

  // ------------------------------------------------------------- transfers
  struct XferRec {
    std::string kind, src, dst;
    double bytes, start, end;
  };
  std::vector<XferRec> xfers_;
  std::map<std::string, double> xfer_bytes_by_kind_;
  std::map<std::string, long> xfer_count_by_kind_;
  double xfer_energy_ = 0;

  double nmp_eff(const std::string& node) const {
    std::string owner;
    if (MemoryTier* m = sys_.memory(node)) owner = m->owner;
    else owner = node;
    ComputeDevice* d = sys_.device(owner);
    if (d && d->type == "NMP") return d->p.get("host_xfer_efficiency");
    return 1.0;
  }

  // elem > 0 表示随机行访问（每行 elem 字节）；zero_copy 表示访存语义直接读取
  void transfer(const std::string& src, const std::string& dst, double bytes, double elem, bool zero_copy,
                const std::string& kind, double outstanding, std::function<void()> done) {
    const Path& p = sys_.route(src, dst);
    if (!p.ok) {
      fail("传输路由失败: " + p.error);
      return;
    }
    if (src == dst || p.links.empty()) {
      at(now_, done);
      return;
    }
    MemoryTier* sm = sys_.memory(src);
    MemoryTier* dm = sys_.memory(dst);
    double moved = bytes;
    double cap = p.bottleneck_bw;
    if (sm && elem > 0) {
      moved = bytes * sm->amplification(elem);
      cap = std::min(cap, sm->random_useful_bw(elem, zero_copy ? outstanding : 512) * sm->amplification(elem));
    }
    if (dm && (dm->kind == MemKind::FLASH || dm->kind == MemKind::SCM)) cap = std::min(cap, dm->seq_write_bw());
    cap *= std::min(nmp_eff(src), nmp_eff(dst));
    double delay = p.latency_s;
    if (zero_copy) {
      cap *= p.zero_copy_eff;
      if (elem > 0) cap = std::min(cap, outstanding * std::max(elem, 64.0) / std::max(p.latency_s, 1e-9));
    } else {
      delay += p.setup_s;
    }
    double pj = p.pj_per_bit + (sm ? sm->pj_per_bit() : 0) + (dm ? dm->pj_per_bit() : 0);
    xfer_energy_ += moved * 8 * pj * 1e-12;
    xfer_bytes_by_kind_[kind] += moved;
    xfer_count_by_kind_[kind]++;
    if (sm) sm->bytes_read += moved;
    if (dm) dm->bytes_written += moved;
    double t0 = now_;
    size_t rec = xfers_.size();
    if (xfers_.size() < max_timeline_) xfers_.push_back({kind, src, dst, moved, t0, t0});
    const Path* pp = &p;
    at(now_ + delay, [this, pp, moved, cap, rec, done]() {
      add_flow(pp, moved, cap, [this, rec, done]() {
        if (rec < xfers_.size()) xfers_[rec].end = now_;
        if (done) done();
      });
    });
  }

  // ------------------------------------------------------------- memory
  std::map<std::string, std::map<int, Copy>> res_;
  struct ChunkRT {
    std::set<std::string> valid;
    std::map<std::string, std::vector<std::function<void()>>> waiters;
    int consumers = 0;
    bool dead = false;
  };
  std::vector<ChunkRT> crt_;

  double chunk_bytes(int ch) const { return g_.chunks[ch].bytes; }
  bool has_copy(const std::string& tier, int ch) const {
    auto it = res_.find(tier);
    return it != res_.end() && it->second.count(ch);
  }
  void place(const std::string& tier, int ch, bool valid) {
    auto& m = res_[tier];
    if (m.count(ch)) return;
    MemoryTier* mt = sys_.memory(tier);
    Copy c;
    c.home = (tier == g_.chunks[ch].home);
    c.valid = valid;
    c.last_use = now_;
    m[ch] = c;
    mt->used += chunk_bytes(ch);
    mt->peak_used = std::max(mt->peak_used, mt->used);
    if (valid) crt_[ch].valid.insert(tier);
  }
  void drop_copy(const std::string& tier, int ch) {
    auto& m = res_[tier];
    auto it = m.find(ch);
    if (it == m.end()) return;
    sys_.memory(tier)->used -= chunk_bytes(ch);
    m.erase(it);
    crt_[ch].valid.erase(tier);
  }
  void mark_valid(const std::string& tier, int ch) {
    if (crt_[ch].dead) {
      drop_copy(tier, ch);
      return;
    }
    auto& m = res_[tier];
    auto it = m.find(ch);
    if (it == m.end()) return;
    it->second.valid = true;
    it->second.last_use = now_;
    crt_[ch].valid.insert(tier);
    auto wit = crt_[ch].waiters.find(tier);
    if (wit != crt_[ch].waiters.end()) {
      auto cbs = std::move(wit->second);
      crt_[ch].waiters.erase(wit);
      for (auto& cb : cbs) cb();
    }
  }
  void when_valid(int ch, const std::string& tier, std::function<void()> cb) {
    if (crt_[ch].valid.count(tier)) {
      cb();
      return;
    }
    crt_[ch].waiters[tier].push_back(std::move(cb));
  }
  // 返回到 dst 带宽最高的有效副本；若无有效副本，返回正在到达的副本（pending=true）
  std::string best_source(int ch, const std::string& dst, bool* pending) {
    *pending = false;
    std::string best;
    double bw = -1;
    for (auto& t : crt_[ch].valid) {
      const Path& p = sys_.route(t, dst);
      if (p.ok && p.bottleneck_bw > bw) {
        bw = p.bottleneck_bw;
        best = t;
      }
    }
    if (!best.empty()) return best;
    for (auto& kv : res_)
      if (kv.first != dst && kv.second.count(ch)) {
        *pending = true;
        return kv.first;
      }
    return "";
  }
  void touch(const std::string& tier, int ch) {
    auto& m = res_[tier];
    auto it = m.find(ch);
    if (it != m.end()) it->second.last_use = now_;
  }
  bool make_room(const std::string& tier, double need, int depth = 0) {
    MemoryTier* m = sys_.memory(tier);
    if (m->capacity_bytes() - m->used >= need) return true;
    if (depth > 3) return false;
    std::vector<std::pair<double, int>> cand;
    for (auto& kv : res_[tier]) {
      const Copy& c = kv.second;
      if (c.pins == 0 && !c.home && c.valid) cand.push_back({c.last_use, kv.first});
    }
    std::sort(cand.begin(), cand.end());
    for (auto& cv : cand) {
      if (m->capacity_bytes() - m->used >= need) break;
      int ch = cv.second;
      if (crt_[ch].valid.size() == 1 && !crt_[ch].dead) {
        // 唯一副本：写回 home
        std::string home = g_.chunks[ch].home;
        if (!has_copy(home, ch)) {
          if (!make_room(home, chunk_bytes(ch), depth + 1)) {
            fail("存储 '" + home + "' 容量不足，无法写回溢出数据 " + g_.chunks[ch].name +
                 "（请调整 placement.spill 到更大的存储层，例如 SCM / CXL 内存 / SSD）");
            return false;
          }
          place(home, ch, false);
          transfer(tier, home, chunk_bytes(ch), 0, false, "writeback", 0, [this, home, ch]() { mark_valid(home, ch); });
        }
      }
      drop_copy(tier, ch);
      evictions_++;
    }
    return m->capacity_bytes() - m->used >= need;
  }
  void consume(int ch) {
    ChunkRT& c = crt_[ch];
    if (--c.consumers > 0 || c.dead || g_.chunks[ch].final_output) return;
    c.dead = true;
    for (auto& kv : res_) {
      auto it = kv.second.find(ch);
      if (it != kv.second.end() && it->second.pins == 0) {
        sys_.memory(kv.first)->used -= chunk_bytes(ch);
        kv.second.erase(it);
      }
    }
    c.valid.clear();
  }
  long evictions_ = 0;

  // ------------------------------------------------------------- tasks
  enum class TS { WAIT, QUEUED, PREP, READY, RUN, DONE };
  struct Remote {
    std::string src;
    double bytes;
    double elem;
    bool zero_copy;
    std::string kind;
  };
  struct TaskRT {
    TS st = TS::WAIT;
    int deps_left = 0;
    std::string dev;
    int pending = 0;
    std::vector<std::pair<std::string, int>> pins;
    double transient = 0;
    std::vector<Remote> remote;
    double remote_seq = 0, remote_acc = 0;
    std::vector<int> children;
    double t_start = 0, t_end = 0;
    int parts = 0;
  };
  std::vector<TaskRT> trt_;
  size_t done_count_ = 0;

  struct DevRT {
    std::set<int> queue, window, ready;
    bool busy = false;
    double est_free = 0;
    bool blocked = false;
  };
  std::map<std::string, DevRT> drt_;
  std::map<std::string, std::deque<int>> copy_q_;
  std::map<std::string, int> copy_inflight_;

  // stats
  struct KRec {
    std::string dev, stage, name;
    double start, end;
  };
  std::vector<KRec> krecs_;
  std::map<AggKey, Agg> agg_;
  std::map<std::string, std::map<std::string, double>> stage_dev_time_;
  std::map<std::string, std::pair<double, double>> stage_span_;
  std::set<std::string> warn_set_;
  std::vector<std::string> warnings_;
  double kernel_energy_ = 0;
  void warn(const std::string& w) {
    if (warn_set_.insert(w).second && warnings_.size() < 50) warnings_.push_back(w);
  }

  bool host_visible(const std::string& tier) const {
    MemoryTier* m = sys_.memory(tier);
    if (!m || !m->byte_addressable()) return false;
    ComputeDevice* o = sys_.device(m->owner);
    return o && (o->type == "CPU" || o->type == "MEM_EXPANDER");
  }
  bool zero_copy_ok(const Task& t, const std::string& dev, const std::string& src) {
    if (src.empty() || !host_visible(src)) return false;
    ComputeDevice* d = sys_.device(dev);
    bool allowed = d->type == "CPU" || t.zero_copy;
    if (!allowed) return false;
    const Path& p = sys_.route(src, dev);
    return p.ok && p.load_store;
  }

  void link_children() {
    for (auto& t : g_.tasks) {
      trt_[t.id].deps_left = static_cast<int>(t.deps.size());
      for (int d : t.deps) trt_[d].children.push_back(t.id);
    }
  }

  void deps_done(int tid) {
    const Task& t = g_.tasks[tid];
    if (t.kind == "barrier") {
      trt_[tid].st = TS::DONE;
      trt_[tid].t_start = trt_[tid].t_end = now_;
      complete(tid);
      return;
    }
    if (t.kind == "copy") {
      copy_q_[t.copy_dst].push_back(tid);
      pump_copies(t.copy_dst);
      return;
    }
    std::string dev = t.dynamic ? choose_device(tid) : t.device;
    enqueue(tid, dev);
  }

  void enqueue(int tid, const std::string& dev) {
    TaskRT& r = trt_[tid];
    r.dev = dev;
    r.st = TS::QUEUED;
    DevRT& d = drt_[dev];
    d.queue.insert(tid);
    ComputeDevice* cd = sys_.device(dev);
    KernelCost kc = cd->estimate(g_.tasks[tid].k, false);
    d.est_free = std::max(d.est_free, now_) + kc.time_s;
    pump(dev);
  }

  std::string choose_device(int tid) {
    const Task& t = g_.tasks[tid];
    std::vector<DeviceState> states;
    double best = 1e300;
    int bi = 0;
    for (size_t i = 0; i < t.candidates.size(); ++i) {
      ComputeDevice* cd = sys_.device(t.candidates[i]);
      DeviceState st;
      st.id = cd->id;
      st.type = cd->type;
      st.now = now_;
      DevRT& d = drt_[cd->id];
      st.busy_until = std::max(now_, d.est_free);
      st.queued = static_cast<int>(d.queue.size() + d.window.size());
      st.est_compute_s = cd->estimate(t.k, false).time_s;
      double miss = 0, bw = 1e18;
      for (auto& in : t.inputs) {
        if (in.mode != TaskInput::FULL) continue;
        if (has_copy(cd->mem->id, in.chunk)) continue;
        miss += chunk_bytes(in.chunk);
        bool pend;
        std::string src = best_source(in.chunk, cd->mem->id, &pend);
        if (!src.empty()) bw = std::min(bw, sys_.route(src, cd->mem->id).bottleneck_bw);
      }
      st.missing_input_bytes = miss;
      st.input_bw = bw >= 1e17 ? 0 : bw;
      double fin = std::max(st.busy_until, now_ + (miss > 0 ? miss / std::max(bw, 1.0) : 0)) + st.est_compute_s;
      if (fin < best) {
        best = fin;
        bi = static_cast<int>(i);
      }
      states.push_back(st);
    }
    if (plugin_) {
      TaskInfo info;
      info.id = tid;
      info.name = t.name;
      info.stage = t.stage;
      info.chunk = t.chunk;
      info.iter = t.iter;
      info.flops = t.k.flops;
      info.bytes = t.k.seq_bytes + t.k.rand_accesses * t.k.rand_elem_bytes;
      int r = plugin_->select_device(info, states);
      if (r >= 0 && r < static_cast<int>(states.size())) bi = r;
    }
    return t.candidates[bi];
  }

  void pump(const std::string& dev) {
    DevRT& d = drt_[dev];
    size_t maxw = s_.overlap ? static_cast<size_t>(s_.prefetch_depth + 1) : 1;
    while (d.window.size() < maxw && !d.queue.empty() && fatal_.empty()) {
      int tid = *d.queue.begin();
      // 先移入窗口再准备：prepare 内部的回调可能重入 pump
      d.queue.erase(d.queue.begin());
      d.window.insert(tid);
      if (!prepare(tid)) {
        d.window.erase(tid);
        d.queue.insert(tid);
        d.blocked = true;
        break;
      }
      d.blocked = false;
    }
    if (!d.busy && !d.ready.empty()) {
      int tid = *d.ready.begin();
      d.ready.erase(d.ready.begin());
      start(tid);
    }
  }
  void pump_blocked() {
    for (auto& kv : drt_)
      if (kv.second.blocked) pump(kv.first);
  }

  void pin(TaskRT& r, const std::string& tier, int ch) {
    res_[tier][ch].pins++;
    r.pins.push_back({tier, ch});
    touch(tier, ch);
  }
  void unpin_all(TaskRT& r) {
    for (auto& pc : r.pins) {
      auto& m = res_[pc.first];
      auto it = m.find(pc.second);
      if (it != m.end()) it->second.pins--;
    }
    r.pins.clear();
  }

  bool prepare(int tid) {
    const Task& t = g_.tasks[tid];
    TaskRT& r = trt_[tid];
    ComputeDevice* cd = sys_.device(r.dev);
    const std::string L = cd->mem->id;
    r.remote.clear();
    r.remote_seq = r.remote_acc = 0;
    r.transient = 0;
    double need = 0;
    std::vector<int> fetch;
    std::map<std::string, double> halo_by_src;
    double reserve = 0;
    std::vector<bool> cache_halo;
    for (auto& in : t.inputs) {
      if (in.mode == TaskInput::FULL) {
        int ch = in.chunk;
        if (has_copy(L, ch)) {
          pin(r, L, ch);
          continue;
        }
        bool pend;
        std::string src = best_source(ch, L, &pend);
        if (src.empty()) {
          fail("内部错误：数据块 " + g_.chunks[ch].name + " 无可用副本（任务 " + t.name + "）");
          return false;
        }
        if (!pend && zero_copy_ok(t, r.dev, src)) {
          r.remote.push_back({src, chunk_bytes(ch), 0, true, "zero_copy"});
          r.remote_seq += chunk_bytes(ch);
          continue;
        }
        need += chunk_bytes(ch);
        fetch.push_back(ch);
      } else {
        // HALO / STREAM：先按“全部成员都不在本地”预留，逐出后再评估
        bool stream = in.elem < 0;
        double whole = 0;
        for (int ch : in.group)
          if (!has_copy(L, ch)) whole += chunk_bytes(ch);
        bool cache = !stream && s_.halo_cache && whole <= 0.5 * cd->mem->capacity_bytes();
        cache_halo.push_back(cache);
        if (cache)
          reserve += whole;
        else
          reserve += stream ? 2 * in.bytes_per_member : in.bytes_per_member * in.group.size();
      }
    }
    for (int o : t.outputs) need += chunk_bytes(o);
    need += reserve;
    MemoryTier* lm = cd->mem;
    if (need > lm->capacity_bytes()) {
      fail("OOM: 任务 '" + t.name + "' 在设备 " + r.dev + " 上需要 " + ParamSet::fmt(need / GB) + " GB 工作内存，超过 " + L +
           " 容量 " + ParamSet::fmt(lm->capacity_bytes() / GB) + " GB（请增大分块数 num_chunks 或更换设备）");
      unpin_all(r);
      return false;
    }
    if (!make_room(L, need)) {
      unpin_all(r);
      return false;
    }
    // 第二遍：评估 HALO / STREAM 成员（此时逐出已完成）
    size_t hidx = 0;
    for (auto& in : t.inputs) {
      if (in.mode != TaskInput::HALO) continue;
      bool cache = cache_halo[hidx++];
      bool stream = in.elem < 0;
      bool any_remote = false;
      for (int ch : in.group) {
        if (has_copy(L, ch)) {
          pin(r, L, ch);
          continue;
        }
        bool pend;
        std::string src = best_source(ch, L, &pend);
        if (src.empty()) {
          fail("内部错误：数据块 " + g_.chunks[ch].name + " 无可用副本");
          unpin_all(r);
          return false;
        }
        any_remote = true;
        if (cache) {
          // halo 缓存：整块拉取到本地（后续轮次与本设备的其它分块可复用）
          fetch.push_back(ch);
        } else if (stream && !pend) {
          r.remote.push_back({src, in.bytes_per_member, 0, false, "stream"});
        } else if (!stream && !pend && zero_copy_ok(t, r.dev, src)) {
          double b = in.accesses_per_member * in.elem;
          r.remote.push_back({src, b, in.elem, true, "zero_copy"});
          r.remote_acc += in.accesses_per_member;
        } else if (pend) {
          r.transient += in.bytes_per_member;
          halo_by_src["#wait:" + std::to_string(ch)] = in.bytes_per_member;
        } else {
          halo_by_src[src] += in.bytes_per_member;
          r.transient += in.bytes_per_member;
        }
      }
      if (stream && any_remote) r.transient += 2 * in.bytes_per_member;
    }
    r.st = TS::PREP;
    r.pending = 1;  // 守卫，防止回调中提前完成
    // 已在本地但仍在到达中的输入
    for (auto& pc : r.pins) {
      if (!crt_[pc.second].valid.count(pc.first)) {
        r.pending++;
        dbg_wait(tid, "pinwait " + g_.chunks[pc.second].name + "@" + pc.first);
        when_valid(pc.second, pc.first, [this, tid, lbl = "pinwait " + g_.chunks[pc.second].name + "@" + pc.first]() { dbg_done(tid, lbl); input_arrived(tid); });
      }
    }
    for (int ch : fetch) {
      place(L, ch, false);
      pin(r, L, ch);
      r.pending++;
      dbg_wait(tid, "fetch " + g_.chunks[ch].name);
      when_valid(ch, L, [this, tid, lbl = "fetch " + g_.chunks[ch].name]() { dbg_done(tid, lbl); input_arrived(tid); });
      bool pend;
      std::string src = best_source(ch, L, &pend);
      auto go = [this, ch, L]() {
        bool p2;
        std::string s2 = best_source(ch, L, &p2);
        if (s2.empty() || p2) {
          fail("内部错误：获取 " + g_.chunks[ch].name + " 失败");
          return;
        }
        transfer(s2, L, chunk_bytes(ch), 0, false, "fetch", 0, [this, ch, L]() { mark_valid(L, ch); });
      };
      if (pend)
        when_valid(ch, src, go);
      else
        go();
    }
    for (auto& kv : halo_by_src) {
      r.pending++;
      if (kv.first.rfind("#wait:", 0) == 0) {
        int ch = std::stoi(kv.first.substr(6));
        bool pend;
        std::string src = best_source(ch, L, &pend);
        double b = kv.second;
        double elem = 0;
        for (auto& in : t.inputs)
          if (in.mode == TaskInput::HALO && in.elem > 0) elem = in.elem;
        dbg_wait(tid, "halowait " + std::to_string(ch));
        when_valid(ch, src, [this, ch, L, b, elem, tid]() {
          bool p2;
          std::string s2 = best_source(ch, L, &p2);
          transfer(s2, L, b, elem, false, "halo", 0, [this, tid, ch]() { dbg_done(tid, "halowait " + std::to_string(ch)); input_arrived(tid); });
        });
      } else {
        double elem = 0;
        for (auto& in : t.inputs)
          if (in.mode == TaskInput::HALO && in.elem > 0) elem = in.elem;
        dbg_wait(tid, "halo " + kv.first);
        transfer(kv.first, L, kv.second, elem, false, "halo", 0, [this, tid, lbl = "halo " + kv.first]() { dbg_done(tid, lbl); input_arrived(tid); });
      }
    }
    for (int o : t.outputs) {
      place(L, o, false);
      pin(r, L, o);
    }
    lm->used += r.transient;
    lm->peak_used = std::max(lm->peak_used, lm->used);
    input_arrived(tid);  // 释放守卫
    return true;
  }

  std::map<int, std::multiset<std::string>> dbg_waits_;
  bool dbg_ = getenv("HGC_DEBUG") != nullptr;
  void dbg_wait(int tid, const std::string& l) {
    if (dbg_) dbg_waits_[tid].insert(l);
  }
  void dbg_done(int tid, const std::string& l) {
    if (!dbg_) return;
    auto it = dbg_waits_[tid].find(l);
    if (it != dbg_waits_[tid].end()) dbg_waits_[tid].erase(it);
  }
  void input_arrived(int tid) {
    TaskRT& r = trt_[tid];
    if (--r.pending > 0) return;
    r.st = TS::READY;
    DevRT& d = drt_[r.dev];
    d.ready.insert(tid);
    if (!d.busy) pump(r.dev);
  }

  void start(int tid) {
    const Task& t = g_.tasks[tid];
    TaskRT& r = trt_[tid];
    ComputeDevice* cd = sys_.device(r.dev);
    DevRT& d = drt_[r.dev];
    d.busy = true;
    r.st = TS::RUN;
    r.t_start = now_;
    Kernel k = t.k;
    k.seq_bytes = std::max(0.0, k.seq_bytes - r.remote_seq);
    k.rand_accesses = std::max(0.0, k.rand_accesses - r.remote_acc);
    KernelCost c = cd->estimate(k, true);
    if (!c.supported) {
      fail("任务 '" + t.name + "' 无法在 " + r.dev + " 上执行: " + c.reason);
      return;
    }
    for (auto& wmsg : c.warnings) warn(wmsg);
    kernel_energy_ += c.energy_j;
    cd->energy_j += c.energy_j;
    cd->kernels++;
    cd->mem->bytes_read += k.seq_bytes;
    AggKey key{r.dev, t.stage, t.k.name};
    Agg& a = agg_[key];
    a.flops += t.k.flops;
    a.bytes += t.k.seq_bytes + t.k.rand_accesses * t.k.rand_elem_bytes;
    a.time += c.time_s;
    a.count++;
    a.op = op_name(t.k.op);
    a.prec = precision_name(t.k.prec);
    r.parts = 1 + static_cast<int>(r.remote.size());
    at(now_ + c.time_s, [this, tid]() { part_done(tid); });
    for (auto& rm : r.remote) {
      std::string dst = rm.zero_copy ? r.dev : cd->mem->id;
      transfer(rm.src, dst, rm.bytes, rm.elem, rm.zero_copy, rm.kind, cd->outstanding(), [this, tid]() { part_done(tid); });
    }
  }

  void part_done(int tid) {
    if (--trt_[tid].parts > 0) return;
    finish(tid);
  }

  void finish(int tid) {
    const Task& t = g_.tasks[tid];
    TaskRT& r = trt_[tid];
    ComputeDevice* cd = sys_.device(r.dev);
    DevRT& d = drt_[r.dev];
    d.busy = false;
    d.window.erase(tid);
    r.t_end = now_;
    double dur = r.t_end - r.t_start;
    cd->busy_s += dur;
    stage_dev_time_[t.stage][r.dev] += dur;
    auto& sp = stage_span_[t.stage];
    if (sp.second == 0 && sp.first == 0) sp.first = r.t_start;
    sp.first = std::min(sp.first, r.t_start);
    sp.second = std::max(sp.second, r.t_end);
    if (krecs_.size() < max_timeline_) krecs_.push_back({r.dev, t.stage, t.name, r.t_start, r.t_end});
    const std::string L = cd->mem->id;
    unpin_all(r);
    cd->mem->used -= r.transient;
    r.transient = 0;
    for (int o : t.outputs) mark_valid(L, o);
    complete(tid);
    pump(r.dev);
    pump_blocked();
  }

  void complete(int tid) {
    const Task& t = g_.tasks[tid];
    TaskRT& r = trt_[tid];
    r.st = TS::DONE;
    done_count_++;
    for (auto& in : t.inputs) {
      if (in.mode == TaskInput::FULL)
        consume(in.chunk);
      else
        for (int ch : in.group) consume(ch);
    }
    for (int o : t.outputs)
      if (crt_[o].consumers == 0 && !g_.chunks[o].final_output) {
        crt_[o].consumers = 1;
        consume(o);
      }
    for (int c : r.children)
      if (--trt_[c].deps_left == 0) deps_done(c);
  }

  void pump_copies(const std::string& dst) {
    auto& q = copy_q_[dst];
    while (copy_inflight_[dst] < 4 && !q.empty()) {
      int tid = q.front();
      q.pop_front();
      const Task& t = g_.tasks[tid];
      int in = t.inputs[0].chunk;
      int out = t.outputs[0];
      if (!make_room(dst, chunk_bytes(out))) {
        fail("暂存目标 '" + dst + "' 容量不足，无法放入 " + g_.chunks[out].name + "（请调整 placement.features / placement.graph）");
        return;
      }
      copy_inflight_[dst]++;
      place(dst, out, false);
      trt_[tid].t_start = now_;
      trt_[tid].st = TS::RUN;
      bool pend;
      std::string src = best_source(in, dst, &pend);
      transfer(src, dst, chunk_bytes(in), 0, false, "stage_in", 0, [this, tid, dst, out]() {
        mark_valid(dst, out);
        trt_[tid].t_end = now_;
        copy_inflight_[dst]--;
        auto& sp = stage_span_["load"];
        if (sp.second == 0 && sp.first == 0) sp.first = trt_[tid].t_start;
        sp.first = std::min(sp.first, trt_[tid].t_start);
        sp.second = std::max(sp.second, now_);
        complete(tid);
        pump_copies(dst);
        pump_blocked();
      });
    }
  }
};

json Sim::run() {
  auto wall0 = Clock::now();
  sys_.reset_stats();
  // 资源容量
  res_cap_.assign(sys_.links.size() * 2, 1.0);
  for (size_t i = 0; i < sys_.links.size(); ++i) {
    double bw = std::max(sys_.links[i]->bw(), 1.0);
    res_cap_[2 * i] = bw;
    res_cap_[2 * i + 1] = bw;
  }
  crt_.assign(g_.chunks.size(), ChunkRT());
  trt_.assign(g_.tasks.size(), TaskRT());
  for (auto& t : g_.tasks)
    for (auto& in : t.inputs) {
      if (in.mode == TaskInput::FULL)
        crt_[in.chunk].consumers++;
      else
        for (int ch : in.group) crt_[ch].consumers++;
    }
  // 初始数据放置
  for (auto& c : g_.chunks) {
    if (c.producer >= 0) continue;
    MemoryTier* m = sys_.memory(c.home);
    if (!m) {
      fail("初始数据 " + c.name + " 的存储 '" + c.home + "' 不存在");
      break;
    }
    if (m->capacity_bytes() - m->used < c.bytes) {
      fail("初始数据放不下：" + c.home + " 容量 " + ParamSet::fmt(m->capacity_bytes() / GB) + " GB 不足以容纳数据集分块 " +
           c.name);
      break;
    }
    place(c.home, c.id, true);
  }
  link_children();
  if (fatal_.empty()) {
    for (auto& t : g_.tasks)
      if (trt_[t.id].deps_left == 0) deps_done(t.id);
  }
  while (!pq_.empty() && fatal_.empty()) {
    Ev e = pq_.top();
    pq_.pop();
    now_ = e.t;
    e.fn();
    if ((++nevents_ & 1023) == 0) {
      double wall = std::chrono::duration<double>(Clock::now() - wall0).count();
      if (wall > wall_limit_) fail("仿真墙钟时间超过上限 " + ParamSet::fmt(wall_limit_) + " s（任务过于细碎，请减少分块数或迭代数）");
    }
  }
  if (fatal_.empty() && done_count_ < g_.tasks.size()) {
    std::string blocked;
    for (auto& kv : drt_)
      if (kv.second.blocked) blocked += kv.first + "(" + sys_.device(kv.first)->mem->id + ") ";
    if (getenv("HGC_DEBUG")) {
      fprintf(stderr, "flows=%zu now=%.6f\n", flows_.size(), now_);
      for (auto& kv : flows_)
        fprintf(stderr, "  flow rate=%.3g rem=%.3g cap=%.3g nres=%zu\n", kv.second.rate, kv.second.remaining, kv.second.cap,
                kv.second.res.size());
      for (auto& kv : drt_) {
        fprintf(stderr, "dev %s busy=%d blocked=%d queue=%zu window=%zu ready=%zu\n", kv.first.c_str(), kv.second.busy,
                kv.second.blocked, kv.second.queue.size(), kv.second.window.size(), kv.second.ready.size());
        for (int t : kv.second.window) {
          fprintf(stderr, "   win task %s st=%d pending=%d\n", g_.tasks[t].name.c_str(), (int)trt_[t].st, trt_[t].pending);
          for (auto& wl : dbg_waits_[t]) fprintf(stderr, "      waiting: %s\n", wl.c_str());
          for (auto& in : g_.tasks[t].inputs) {
            std::vector<int> chs = in.mode == TaskInput::FULL ? std::vector<int>{in.chunk} : in.group;
            for (int ch : chs) {
              std::string where;
              for (auto& rk : res_)
                if (rk.second.count(ch))
                  where += rk.first + (rk.second.at(ch).valid ? "(v) " : "(-) ");
              if (where.find("(v)") == std::string::npos)
                fprintf(stderr, "      input %s at: %s waiters=%zu\n", g_.chunks[ch].name.c_str(), where.c_str(),
                        crt_[ch].waiters.size());
            }
          }
        }
        if (!kv.second.queue.empty()) fprintf(stderr, "   head %s\n", g_.tasks[*kv.second.queue.begin()].name.c_str());
      }
      for (auto& kv : res_) {
        MemoryTier* m = sys_.memory(kv.first);
        fprintf(stderr, "tier %s used %.2f/%.2f GB copies=%zu\n", kv.first.c_str(), m->used / GB, m->capacity_bytes() / GB,
                kv.second.size());
        int shown = 0;
        for (auto& c : kv.second)
          if (shown++ < 12)
            fprintf(stderr, "   %s home=%d valid=%d pins=%d %.3fGB dead=%d\n", g_.chunks[c.first].name.c_str(), c.second.home,
                    c.second.valid, c.second.pins, chunk_bytes(c.first) / GB, (int)crt_[c.first].dead);
      }
    }
    fail("仿真停滞：" + std::to_string(g_.tasks.size() - done_count_) + " 个任务未完成。" +
         (blocked.empty() ? std::string("") : "存储容量不足而阻塞的设备: " + blocked + "——请增大分块数、调整溢出层或更换设备"));
  }
  double wall = std::chrono::duration<double>(Clock::now() - wall0).count();
  double T = now_;

  json res;
  res["status"] = fatal_.empty() ? "ok" : "error";
  if (!fatal_.empty()) res["errors"] = json::array({fatal_});
  res["warnings"] = warnings_;
  res["makespan_s"] = T;
  res["sim_events"] = nevents_;
  res["wall_time_s"] = wall;
  res["tasks_done"] = done_count_;
  res["graph"] = g_.summary;
  res["evictions"] = evictions_;
  // stages
  json stages = json::object();
  for (auto& kv : stage_span_) {
    json byd = json::object();
    double tot = 0;
    for (auto& d : stage_dev_time_[kv.first]) {
      byd[d.first] = d.second;
      tot += d.second;
    }
    stages[kv.first] = {{"start_s", kv.second.first}, {"end_s", kv.second.second},
                        {"span_s", kv.second.second - kv.second.first}, {"device_time_s", byd}, {"kernel_time_s", tot}};
  }
  res["stages"] = stages;
  // devices
  double idle_e = 0;
  json devs = json::array();
  for (auto& d : sys_.devices) {
    double ie = d->idle_w() * T;
    idle_e += ie;
    devs.push_back({{"id", d->id},
                    {"type", d->type},
                    {"busy_s", d->busy_s},
                    {"utilization", T > 0 ? d->busy_s / T : 0},
                    {"kernels", d->kernels},
                    {"dynamic_energy_j", d->energy_j},
                    {"idle_energy_j", ie},
                    {"has_compute", d->has_compute()}});
    if (auto* cim = dynamic_cast<CIMDevice*>(d.get())) devs.back()["endurance_used"] = cim->endurance_used();
  }
  res["devices"] = devs;
  double mem_static = 0;
  json mems = json::array();
  for (auto& m : sys_.memories) {
    mem_static += m->static_power_w() * T;
    mems.push_back({{"id", m->id},
                    {"kind", mem_kind_name(m->kind)},
                    {"capacity_GB", m->capacity_bytes() / GB},
                    {"peak_used_GB", m->peak_used / GB},
                    {"read_GB", m->bytes_read / GB},
                    {"written_GB", m->bytes_written / GB}});
  }
  res["memories"] = mems;
  json links = json::array();
  for (auto& l : sys_.links) {
    double bw = l->bw();
    double util = 0;
    if (T > 0 && bw > 0)
      util = l->full_duplex() ? std::max(l->bytes_fwd, l->bytes_rev) / (bw * T) : (l->bytes_fwd + l->bytes_rev) / (bw * T);
    links.push_back({{"id", l->id},
                     {"kind", link_kind_name(l->kind)},
                     {"a", l->a},
                     {"b", l->b},
                     {"bw_GBps", bw / GBps},
                     {"up_GB", l->bytes_fwd / GB},
                     {"down_GB", l->bytes_rev / GB},
                     {"utilization", std::min(1.0, util)}});
  }
  res["links"] = links;
  json xk = json::object();
  double xtot = 0;
  for (auto& kv : xfer_bytes_by_kind_) {
    xk[kv.first] = {{"GB", kv.second / GB}, {"count", xfer_count_by_kind_[kv.first]}};
    xtot += kv.second;
  }
  res["transfers"] = {{"total_GB", xtot / GB}, {"by_kind", xk}};
  double sw_static = 10.0 * sys_.switches.size() * T;
  res["energy"] = {{"compute_j", kernel_energy_},
                   {"transfer_j", xfer_energy_},
                   {"static_j", idle_e + mem_static + sw_static},
                   {"total_j", kernel_energy_ + xfer_energy_ + idle_e + mem_static + sw_static},
                   {"avg_power_w", T > 0 ? (kernel_energy_ + xfer_energy_ + idle_e + mem_static + sw_static) / T : 0}};
  // roofline
  json rl = json::array();
  for (auto& kv : agg_) {
    ComputeDevice* d = sys_.device(kv.first.dev);
    Precision pr;
    parse_precision(kv.second.prec, pr);
    OpClass op = OpClass::GEMM;
    if (kv.second.op != "GEMM") op = OpClass::ELEMWISE;
    rl.push_back({{"device", kv.first.dev},
                  {"stage", kv.first.stage},
                  {"kernel", kv.first.name},
                  {"op", kv.second.op},
                  {"precision", kv.second.prec},
                  {"count", kv.second.count},
                  {"GFLOP", kv.second.flops / 1e9},
                  {"GB", kv.second.bytes / GB},
                  {"time_s", kv.second.time},
                  {"arith_intensity", kv.second.bytes > 0 ? kv.second.flops / kv.second.bytes : 0},
                  {"achieved_GFLOPS", kv.second.time > 0 ? kv.second.flops / kv.second.time / 1e9 : 0},
                  {"peak_GFLOPS", d->peak_flops(pr, op) / 1e9},
                  {"mem_bw_GBps", d->mem ? d->mem->seq_bw() / GBps : 0}});
  }
  res["roofline"] = rl;
  json kt = json::array();
  for (auto& k : krecs_) kt.push_back({k.dev, k.stage, k.name, k.start, k.end});
  json xt = json::array();
  for (auto& x : xfers_) xt.push_back({x.kind, x.src, x.dst, x.bytes, x.start, x.end});
  res["timeline"] = {{"kernels", kt},
                     {"transfers", xt},
                     {"columns_kernels", {"device", "stage", "name", "start", "end"}},
                     {"columns_transfers", {"kind", "src", "dst", "bytes", "start", "end"}},
                     {"truncated", krecs_.size() >= max_timeline_ || xfers_.size() >= max_timeline_}};
  res["budget"] = {{"enabled", sys_.budget_enabled},
                   {"cost_usd", sys_.total_cost()},
                   {"tdp_w", sys_.total_tdp()},
                   {"cost_budget_usd", sys_.cost_budget_usd},
                   {"power_budget_w", sys_.power_budget_w}};
  return res;
}

// ---------------------------------------------------------------------------
// 插件视图实现
// ---------------------------------------------------------------------------
class SystemViewImpl : public SystemView {
 public:
  SystemViewImpl(System& s, const Workload& w) : sys_(s), w_(w) {}
  json describe() const override { return sys_.describe(); }
  std::vector<std::string> compute_devices() const override {
    std::vector<std::string> v;
    for (auto& d : sys_.devices)
      if (d->has_compute()) v.push_back(d->id);
    return v;
  }
  std::string device_type(const std::string& dev) const override {
    ComputeDevice* d = sys_.device(dev);
    return d ? d->type : "";
  }
  std::string host_id() const override { return sys_.host()->id; }
  std::string local_memory(const std::string& dev) const override {
    ComputeDevice* d = sys_.device(dev);
    return d && d->mem ? d->mem->id : "";
  }
  double memory_capacity(const std::string& mem) const override {
    MemoryTier* m = sys_.memory(mem);
    if (!m)
      if (ComputeDevice* d = sys_.device(mem)) m = d->mem;
    return m ? m->capacity_bytes() : 0;
  }
  double stage_time(const std::string& stage, const std::string& dev, const std::string& precision) const override {
    ComputeDevice* d = sys_.device(dev);
    Precision p;
    if (!d || !d->has_compute() || !parse_precision(precision, p)) return -1;
    d->reset_state();
    double t = 0;
    for (auto& k : stage_kernels(w_, stage, p)) {
      KernelCost c = d->estimate(k, true);
      if (!c.supported) {
        d->reset_state();
        return -1;
      }
      t += c.time_s;
    }
    d->reset_state();
    return t;
  }
  double path_bandwidth(const std::string& src, const std::string& dst) const override {
    const Path& p = sys_.route(src, dst);
    return p.ok ? p.bottleneck_bw : 0;
  }
  double path_latency(const std::string& src, const std::string& dst) const override {
    const Path& p = sys_.route(src, dst);
    return p.ok ? p.latency_s : -1;
  }
  bool path_load_store(const std::string& src, const std::string& dst) const override {
    const Path& p = sys_.route(src, dst);
    return p.ok && p.load_store;
  }

 private:
  System& sys_;
  const Workload& w_;
};

class WorkloadViewImpl : public WorkloadView {
 public:
  WorkloadViewImpl(const Workload& w, const System& s) : w_(w), sys_(s) {}
  json describe() const override { return w_.to_json(); }
  json characterize() const override { return workload_characterize(w_); }
  std::string dataset_location() const override { return default_dataset_location(sys_); }

 private:
  const Workload& w_;
  const System& sys_;
};

struct PluginHandle {
  void* h = nullptr;
  IScheduler* s = nullptr;
  void (*destroy)(IScheduler*) = nullptr;
  ~PluginHandle() {
    if (s && destroy) destroy(s);
    // 不 dlclose：避免插件中静态对象析构顺序问题
  }
};

bool load_plugin(const std::string& path, PluginHandle& ph, std::string& err) {
  ph.h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!ph.h) {
    err = std::string("无法加载调度插件 '") + path + "': " + dlerror();
    return false;
  }
  using Create = IScheduler* (*)();
  using Abi = int (*)();
  auto abi = reinterpret_cast<Abi>(dlsym(ph.h, "hgc_scheduler_abi_version"));
  auto create = reinterpret_cast<Create>(dlsym(ph.h, "hgc_create_scheduler"));
  ph.destroy = reinterpret_cast<void (*)(IScheduler*)>(dlsym(ph.h, "hgc_destroy_scheduler"));
  if (!create || !abi || !ph.destroy) {
    err = "插件 '" + path + "' 缺少导出符号（请使用 HGC_EXPORT_SCHEDULER 宏）";
    return false;
  }
  if (abi() != HGC_SCHEDULER_ABI) {
    err = "插件 ABI 版本不匹配";
    return false;
  }
  ph.s = create();
  return ph.s != nullptr;
}

json error_result(const std::vector<std::string>& errors) {
  return {{"status", "error"}, {"errors", errors}};
}

// 处理 spill = auto
void resolve_auto_spill(Strategy& s, const Workload& w, const System& sys) {
  auto it = s.placement.find("spill");
  if (it != s.placement.end() && it->second != "auto") return;
  double h = w.alg.encoder_dims.back();
  double E = w.ds.has_graph ? w.ds.edges : w.ds.nodes * w.ds.knn_k;
  double foot = w.ds.nodes * h * 4 * 3 + E * (w.ds.index_bytes + 4);
  MemoryTier* dram = sys.host()->mem;
  if (foot < 0.5 * dram->capacity_bytes()) {
    s.placement["spill"] = dram->id;
    return;
  }
  // 选择主机可见的最大容量字节寻址存储，否则选数据集所在闪存
  std::string best;
  double cap = dram->capacity_bytes();
  for (auto& m : sys.memories) {
    ComputeDevice* o = sys.device(m->owner);
    if (!o) continue;
    bool hv = m->byte_addressable() && (o->type == "CPU" || o->type == "MEM_EXPANDER");
    if (hv && m->capacity_bytes() > cap) {
      cap = m->capacity_bytes();
      best = m->id;
    }
  }
  if (best.empty() || foot > 0.8 * cap) {
    ComputeDevice* dd = sys.device(default_dataset_location(sys));
    if (dd && dd->mem) best = dd->mem->id;
  }
  s.placement["spill"] = best.empty() ? dram->id : best;
}

struct Prepared {
  std::unique_ptr<System> sys;
  Workload w;
  Strategy s;
  PluginHandle plugin;
  json resolved;
};

bool prepare_request(const json& req, Prepared& P, std::vector<std::string>& errors) {
  if (!req.contains("system") || !req.contains("workload") || !req.contains("strategy")) {
    errors.push_back("请求必须包含 system / workload / strategy");
    return false;
  }
  P.sys = System::from_json(req["system"], errors);
  if (!errors.empty()) return false;
  if (!Workload::from_json(req["workload"], P.w, errors)) return false;
  json sj = req["strategy"];
  std::string plugin = sj.value("plugin", "");
  if (!plugin.empty()) {
    std::string err;
    if (!load_plugin(plugin, P.plugin, err)) {
      errors.push_back(err);
      return false;
    }
    SystemViewImpl sv(*P.sys, P.w);
    WorkloadViewImpl wv(P.w, *P.sys);
    json planned;
    try {
      planned = P.plugin.s->plan(sv, wv, sj.value("plugin_args", json::object()), sj);
    } catch (std::exception& ex) {
      errors.push_back(std::string("插件 plan() 抛出异常: ") + ex.what());
      return false;
    }
    if (!planned.is_object()) {
      errors.push_back("插件 plan() 必须返回 JSON 对象");
      return false;
    }
    planned["plugin"] = plugin;
    if (!planned.contains("name")) planned["name"] = P.plugin.s->name();
    sj = planned;
  }
  if (!Strategy::from_json(sj, P.s, errors)) return false;
  resolve_auto_spill(P.s, P.w, *P.sys);
  P.resolved = P.s.to_json();
  if (sj.contains("planner_notes")) P.resolved["planner_notes"] = sj["planner_notes"];
  if (sj.contains("description")) P.resolved["description"] = sj["description"];
  return true;
}

}  // namespace

json simulate(const json& req) {
  std::vector<std::string> errors;
  Prepared P;
  try {
    if (!prepare_request(req, P, errors)) return error_result(errors);
    TaskGraph g;
    if (!build_task_graph(P.w, P.s, *P.sys, g, errors)) {
      json r = error_result(errors);
      r["strategy"] = P.resolved;
      return r;
    }
    json opts = req.value("options", json::object());
    Sim sim(*P.sys, P.w, P.s, g, P.plugin.s, opts);
    json r = sim.run();
    P.resolved["partition"]["num_chunks"] = g.P;
    r["strategy"] = P.resolved;
    r["system_name"] = P.sys->name;
    r["workload_name"] = P.w.name;
    return r;
  } catch (std::exception& ex) {
    errors.push_back(std::string("异常: ") + ex.what());
    return error_result(errors);
  }
}

json validate_system(const json& system) {
  std::vector<std::string> errors;
  try {
    auto sys = System::from_json(system, errors);
    json r;
    r["ok"] = errors.empty();
    r["errors"] = errors;
    if (sys) r["system"] = sys->describe();
    return r;
  } catch (std::exception& ex) {
    return {{"ok", false}, {"errors", {std::string("异常: ") + ex.what()}}};
  }
}

json characterize(const json& wj) {
  std::vector<std::string> errors;
  Workload w;
  if (!Workload::from_json(wj, w, errors)) return {{"ok", false}, {"errors", errors}};
  json r = workload_characterize(w);
  r["ok"] = true;
  return r;
}

json plan_strategy(const json& req) {
  std::vector<std::string> errors;
  Prepared P;
  try {
    if (!prepare_request(req, P, errors)) return {{"ok", false}, {"errors", errors}};
    TaskGraph g;
    if (!build_task_graph(P.w, P.s, *P.sys, g, errors)) return {{"ok", false}, {"errors", errors}, {"strategy", P.resolved}};
    P.resolved["partition"]["num_chunks"] = g.P;
    return {{"ok", true}, {"strategy", P.resolved}, {"graph", g.summary}};
  } catch (std::exception& ex) {
    return {{"ok", false}, {"errors", {std::string("异常: ") + ex.what()}}};
  }
}

}  // namespace hgc
