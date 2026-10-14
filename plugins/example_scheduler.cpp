// =============================================================================
// example_scheduler.cpp : 调度插件模板（同学们从这里开始写自己的 C++ 调度器）
//
// 使用方法：
//   1. 复制本文件到 plugins/student/my_scheduler.cpp（文件名即插件名）
//   2. 编译并为这次 build 命名（添加插件后必须命名，名称不能用保留名）：
//        make build BUILD_NAME=my_v1
//      生成 build/plugins/libmy_scheduler.so；如果前端正在运行，请停止后重新运行 bash scripts/run_frontend.sh
//   3. 新建策略 configs/user/strategies/mine.json（或在前端“任务与策略”页编辑后保存）：
//        { "name": "mine", "plugin": "build/plugins/libmy_scheduler.so",
//          "plugin_args": { "任意参数": 1 } }
//   4. 在网页前端“任务与策略”页选择该策略运行，或 bash scripts/run_suite.sh --strategies mine
//
// 本示例实现一个“单设备最快”策略：每个阶段只选 stage_time 最小的一个设备，
// 不做多设备并行、不开流水线 —— 仅用于演示 API，性能很有限，答题时请在此基础上改进。
// =============================================================================
#include <limits>

#include "hgcsim/scheduler.hpp"

using nlohmann::json;

class ExampleScheduler : public hgc::IScheduler {
 public:
  std::string name() const override { return "example-fastest-single-device"; }

  json plan(const hgc::SystemView& sys, const hgc::WorkloadView& wl, const json& args, const json& base) override {
    // 可以查看负载画像：各阶段 GFLOP、访存量、算术强度
    json ch = wl.characterize();
    (void)ch;
    (void)args;

    // 对每个阶段在所有设备 × 精度上找估计时间最短的组合
    auto fastest = [&](const std::string& stage, std::string& best_prec) {
      std::string best;
      double bt = std::numeric_limits<double>::max();
      for (const auto& dev : sys.compute_devices()) {
        for (const char* p : {"fp32", "fp16"}) {
          double t = sys.stage_time(stage, dev, p);  // 数据已在本地时的计算时间；-1 表示不支持
          if (t >= 0 && t < bt) {
            bt = t;
            best = dev;
            best_prec = p;
          }
        }
      }
      return best;
    };
    std::string p_fe, p_gc, p_cl;
    std::string fe = fastest("feature_extraction", p_fe);
    std::string gc = fastest("graph_construction", p_gc);
    std::string cl = fastest("clustering", p_cl);

    json st = base;  // 以用户给出的策略 JSON 为默认值
    st["name"] = name();
    st["partition"] = {{"num_chunks", "auto"}, {"method", "range"}};
    st["pipeline"] = {{"overlap", false}, {"prefetch_depth", 1}};
    st["stages"] = {{"feature_extraction", {{"devices", {fe}}, {"precision", p_fe}}},
                    {"graph_construction", {{"devices", {gc}}, {"precision", p_gc}}},
                    {"clustering", {{"devices", {cl}}, {"precision", p_cl}}}};
    return st;
  }

  // 若某阶段配置为 "assign": "dynamic"，每个分块任务就绪时会回调这里。
  // 返回 candidates 的下标；返回 -1 使用模拟器内置的最早完成时间策略。
  int select_device(const hgc::TaskInfo& task, const std::vector<hgc::DeviceState>& candidates) override {
    (void)task;
    (void)candidates;
    return -1;
  }
};

HGC_EXPORT_SCHEDULER(ExampleScheduler)
