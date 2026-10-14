// HGCSim 单元测试 / 标定测试（无第三方测试框架）
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

#include "hgcsim/runtime.hpp"
#include "hgcsim/system.hpp"
#include "hgcsim/workload.hpp"

using namespace hgc;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (cond) {                                                       \
      ++g_pass;                                                       \
    } else {                                                          \
      ++g_fail;                                                       \
      std::cerr << "[FAIL] " << msg << "  (" << #cond << ")\n";       \
    }                                                                 \
  } while (0)
#define NEAR(a, b, rel) (std::fabs((a) - (b)) <= (rel) * std::fabs(b))

static json load(const std::string& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return json::parse(ss.str());
}

static json small_workload(double n = 100000, double e = 2000000) {
  return {{"name", "unit"},
          {"dataset", {{"nodes", n}, {"edges", e}, {"feature_dim", 128}, {"clusters", 16}, {"has_graph", true}}},
          {"algorithm", {{"encoder_dims", {128, 64}}, {"iterations", 5}, {"prop_hops", 2}}}};
}

static json strategy(const std::string& fe, const std::string& gc, const std::string& cl, int chunks = 0,
                     const std::string& prec = "fp32") {
  return {{"name", "t"},
          {"partition", {{"num_chunks", chunks == 0 ? json("auto") : json(chunks)}, {"method", "range"}}},
          {"stages",
           {{"feature_extraction", {{"devices", {fe}}, {"precision", prec}}},
            {"graph_construction", {{"devices", {gc}}}},
            {"clustering", {{"devices", {cl}}, {"precision", prec}}}}}};
}

int main() {
  // ---------------- 互连带宽推导 ----------------
  {
    Link l("x", LinkKind::PCIE);
    l.p.set("gen", 4);
    l.p.set("lanes", 16);
    CHECK(l.bw() > 24e9 && l.bw() < 27e9, "PCIe 4.0 x16 有效带宽应约 25 GB/s，实际 " << l.bw() / 1e9);
    l.p.set("gen", 5);
    CHECK(l.bw() > 48e9 && l.bw() < 54e9, "PCIe 5.0 x16 有效带宽应约 50 GB/s");
    l.p.set("gen", 7);
    CHECK(l.raw_bw() == 256e9, "PCIe 7.0 x16 原始带宽 256 GB/s");
    Link c("c", LinkKind::CXL);
    c.p.set("version", 2.0);
    c.p.set("lanes", 16);
    CHECK(NEAR(c.raw_bw(), 64e9, 1e-9), "CXL 2.0 x16 原始带宽 64 GB/s");
    c.p.set("version", 4.0);
    c.p.set("bundled_ports", 2);
    CHECK(NEAR(c.raw_bw(), 512e9, 1e-9), "CXL 4.0 x16 ×2 捆绑端口 = 512 GB/s");
    std::vector<std::string> err;
    c.p.set("version", 3.0);
    c.validate(err);
    CHECK(!err.empty(), "捆绑端口在 CXL 3.0 上应报错");
  }
  // ---------------- 存储推导 ----------------
  {
    MemoryTier d("d", MemKind::DRAM);
    CHECK(NEAR(d.peak_bw(), 89.6e9, 1e-9), "DDR5-5600 双通道峰值 89.6 GB/s");
    MemoryTier h("h", MemKind::HBM);
    h.p.set("stacks", 8);
    h.p.set("interface_bits", 2048);
    h.p.set("gbps_per_pin", 10.74);
    h.p.set("gb_per_stack", 36);
    CHECK(h.peak_bw() > 21.9e12 && h.peak_bw() < 22.1e12, "8 堆栈 HBM4 @10.74Gbps ≈ 22 TB/s (Rubin 级)");
    CHECK(NEAR(h.capacity_bytes(), 288 * GB, 1e-9), "HBM4 8×36GB = 288 GB");
    std::vector<std::string> err;
    h.p.set("interface_bits", 1024);
    h.validate(err);
    CHECK(!err.empty(), "HBM3E (1024bit) 超过 9.8Gbps 应报错");
    MemoryTier f("f", MemKind::FLASH);
    CHECK(f.seq_read_bw() > 10e9 && f.seq_read_bw() < 14e9, "默认 NVMe 内部顺序读带宽");
    CHECK(f.random_useful_bw(4096, 1e6) <= 1.2e6 * 4096 * 1.0001, "闪存随机读受 IOPS 限制");
    CHECK(f.amplification(256) == 16, "256B 随机读在闪存上放大 16 倍");
    ParamSet ps;
    ps.add("x", 1, 0, 2, "", "");
    CHECK(!ps.set("x", 3).empty(), "超出上界应被拒绝");
  }
  // ---------------- 设备峰值推导 ----------------
  {
    GPUDevice g("g");
    CHECK(NEAR(g.peak_flops(Precision::FP32, OpClass::GEMM), 29.1456e12, 1e-6), "默认 GPU FP32 峰值 ≈ 29.1 TFLOPS");
    CPUDevice c("c");
    CHECK(NEAR(c.peak_flops(Precision::FP32, OpClass::GEMM), 1.024e12, 1e-9), "默认 CPU FP32 峰值 1.024 TFLOPS");
    TPUDevice t("t");
    t.p.set("mxu_count", 8);
    t.p.set("mxu_dim", 256);
    t.p.set("freq_ghz", 2.2);
    t.p.set("fp8_ratio", 2);
    CHECK(NEAR(t.peak_flops(Precision::FP8, OpClass::GEMM), 4.614e15, 0.01), "Ironwood 级 TPU FP8 ≈ 4614 TFLOPS");
    Kernel k;
    k.op = OpClass::GEMM;
    k.gemm_k = 64;
    k.gemm_n = 7;
    k.prec = Precision::BF16;
    CHECK(t.shape_efficiency(k) < 0.02, "脉动阵列在 n=7 的窄 GEMM 上利用率极低");
    CIMDevice r("r", false);
    Kernel sp;
    sp.op = OpClass::SPMM;
    CHECK(!r.supports(sp, nullptr), "RRAM 存算一体不支持 SpMM");
    Kernel gm;
    gm.op = OpClass::GEMM;
    gm.prec = Precision::INT8;
    gm.gemm_m = 1e5;
    gm.gemm_k = 128;
    gm.gemm_n = 64;
    gm.flops = 2 * 1e5 * 128 * 64;
    gm.weight_id = "W";
    KernelCost c1 = r.estimate(gm, true);
    KernelCost c2 = r.estimate(gm, true);
    CHECK(c1.extra_s > 0 && c2.extra_s == 0, "静态权重只需编程一次");
    CIMDevice m("m", true);
    KernelCost m1 = m.estimate(gm, true);
    CHECK(m1.extra_s < c1.extra_s, "MRAM 写入比 RRAM 快");
    FPGADevice fp("f");
    fp.p.set("max_resident_kernels", 1);
    Kernel a, b;
    a.op = OpClass::SORT;
    b.op = OpClass::SPMM;
    a.flops = b.flops = 1e6;
    fp.estimate(a, true);
    KernelCost fb = fp.estimate(b, true);
    KernelCost fa = fp.estimate(a, true);
    CHECK(fb.extra_s > 0 && fa.extra_s > 0, "FPGA 只驻留 1 个内核时交替执行需要重配置");
  }
  // ---------------- 所有参数默认值都在上下界内 ----------------
  {
    json cat = catalog();
    int bad = 0;
    auto scan = [&](const json& specs, const std::string& who) {
      for (auto& sp : specs) {
        double v = sp["default"], mn = sp["min"], mx = sp["max"];
        if (v < mn || v > mx) {
          ++bad;
          std::cerr << "  默认值越界: " << who << "." << sp["name"].get<std::string>() << "\n";
        }
      }
    };
    for (auto it = cat["devices"].begin(); it != cat["devices"].end(); ++it) scan(it.value()["params"], it.key());
    for (auto it = cat["memories"].begin(); it != cat["memories"].end(); ++it) scan(it.value(), it.key());
    for (auto it = cat["links"].begin(); it != cat["links"].end(); ++it) scan(it.value(), it.key());
    for (auto it = cat["switches"].begin(); it != cat["switches"].end(); ++it) scan(it.value(), it.key());
    CHECK(bad == 0, "所有组件参数默认值都应在上下界内");
  }
  // ---------------- 系统配置与路由 ----------------
  json pc = load("configs/systems/default_pc.json");
  json lab = load("configs/systems/hetero_lab.json");
  {
    json r = validate_system(pc);
    CHECK(r["ok"].get<bool>(), "default_pc 配置应合法");
    r = validate_system(lab);
    CHECK(r["ok"].get<bool>(), "hetero_lab 配置应合法: " << r["errors"].dump());
    json bad = pc;
    bad["devices"][0]["params"]["sms"] = 300;
    CHECK(!validate_system(bad)["ok"].get<bool>(), "GPU SM 数超过上界应报错");
    bad = pc;
    bad["devices"][0]["attach"]["link"]["params"]["gen"] = 6;
    CHECK(!validate_system(bad)["ok"].get<bool>(), "PCIe 代数超过设备能力应报错");
    bad = lab;
    bad["switches"][0]["attach"]["link"]["params"]["version"] = 1.1;
    CHECK(!validate_system(bad)["ok"].get<bool>(), "CXL 1.1 不支持交换机");
    bad = lab;
    bad["budget"] = {{"enabled", true}, {"power_w", 500}, {"cost_usd", 1000}};
    CHECK(!validate_system(bad)["ok"].get<bool>(), "超出预算应报错");
    std::vector<std::string> err;
    auto sys = System::from_json(lab, err);
    const Path& p = sys->route("cxlmem0.mem", "npu0");
    bool via_host = false;
    for (auto& n : p.nodes) via_host |= (n == "cpu0");
    CHECK(p.ok && !via_host && p.load_store, "CXL 3.0 交换机下设备可直接 P2P 访存");
    json v2 = lab;
    for (auto& d : v2["devices"])
      if (d["attach"]["to"] == "cxlsw0") d["attach"]["link"]["params"]["version"] = 2.0;
    err.clear();
    auto sys2 = System::from_json(v2, err);
    const Path& p2 = sys2->route("cxlmem0.mem", "npu0");
    via_host = false;
    for (auto& n : p2.nodes) via_host |= (n == "cpu0");
    CHECK(p2.ok && via_host, "CXL 2.0 交换机下 P2P 需绕行主机");
    const Path& p3 = sys->route("cxlmem0.mem", "cpu0");
    CHECK(p3.latency_s > 2e-7 && p3.latency_s < 6e-7, "经 CXL 交换机访存延迟在 200~600ns 量级: " << p3.latency_s);
  }
  // ---------------- 端到端仿真 ----------------
  {
    json req = {{"system", pc}, {"workload", small_workload()}, {"strategy", strategy("cpu0", "cpu0", "cpu0")}};
    json r1 = simulate(req);
    CHECK(r1["status"] == "ok", "CPU 基线应成功: " << r1.value("errors", json()).dump());
    req["strategy"] = strategy("gpu0", "cpu0", "gpu0");
    json r2 = simulate(req);
    CHECK(r2["status"] == "ok", "GPU 基线应成功");
    CHECK(r2["makespan_s"].get<double>() < r1["makespan_s"].get<double>(), "GPU 卸载应快于纯 CPU");
    // 链路越慢，传输越多的策略越慢
    json slow = pc;
    slow["devices"][0]["attach"]["link"]["params"]["gen"] = 1;
    req["system"] = slow;
    json r3 = simulate(req);
    CHECK(r3["makespan_s"].get<double>() > r2["makespan_s"].get<double>(), "PCIe Gen1 应慢于 Gen4");
    // OOM
    json tiny = pc;
    tiny["devices"][0]["memory"]["params"]["capacity_gb"] = 1;
    json big = small_workload(5e6, 1e8);
    json r4 = simulate({{"system", tiny}, {"workload", big}, {"strategy", strategy("gpu0", "cpu0", "gpu0", 1)}});
    CHECK(r4["status"] == "error", "单分块放不进 1GB 显存应报 OOM");
    json r5 = simulate({{"system", tiny}, {"workload", big}, {"strategy", strategy("gpu0", "cpu0", "gpu0")}});
    CHECK(r5["status"] == "ok", "自动分块后应能运行: " << r5.value("errors", json()).dump());
    // 不支持的映射
    json r6 = simulate({{"system", lab}, {"workload", small_workload()}, {"strategy", strategy("cpu0", "cpu0", "rram0", 0, "int8")}});
    CHECK(r6["status"] == "error", "SpMM 映射到 RRAM 应报错");
    // 零拷贝：CXL 内存
    json zc = strategy("gpu0", "cpu0", "npu0", 4, "fp16");
    zc["placement"] = {{"spill", "cxlmem0.mem"}};
    zc["stages"]["clustering"]["zero_copy"] = true;
    json r7 = simulate({{"system", lab}, {"workload", small_workload(2e6, 4e7)}, {"strategy", zc}});
    CHECK(r7["status"] == "ok", "CXL 零拷贝策略应成功: " << r7.value("errors", json()).dump());
    if (r7["status"] == "ok") CHECK(r7["transfers"]["by_kind"].contains("zero_copy"), "应产生零拷贝流量");
    // 流水线重叠不应变慢
    json ov = strategy("gpu0", "cpu0", "gpu0", 8);
    json r8 = simulate({{"system", pc}, {"workload", small_workload(2e6, 4e7)}, {"strategy", ov}});
    ov["pipeline"] = {{"overlap", true}, {"prefetch_depth", 2}};
    json r9 = simulate({{"system", pc}, {"workload", small_workload(2e6, 4e7)}, {"strategy", ov}});
    CHECK(r9["makespan_s"].get<double>() <= r8["makespan_s"].get<double>() * 1.0001, "开启流水线重叠不应更慢");
  }
  std::cout << "通过 " << g_pass << " 项，失败 " << g_fail << " 项\n";
  return g_fail == 0 ? 0 : 1;
}
