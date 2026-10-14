// =============================================================================
// scheduler.hpp : C++ 调度器插件接口（进阶答题接口）
//
// 答题同学可以实现 IScheduler，编译为共享库（.so），在策略 JSON 中通过
//   { "plugin": "build/plugins/libmy_scheduler.so", "plugin_args": {...} }
// 加载。插件有两个扩展点：
//   1) plan()          —— 静态规划：根据系统与负载信息，返回一份策略 JSON
//                         （与 JSON 策略格式完全相同，见 docs/STUDENT_GUIDE.md）
//   2) select_device() —— 动态调度：对 "assign":"dynamic" 的阶段，每个分块任务
//                         依赖满足时回调，由插件从候选设备中挑选执行设备
// 插件只依赖本头文件（header-only 的 nlohmann/json），无需链接模拟器库。
// 使用宏 HGC_EXPORT_SCHEDULER(类名) 导出工厂函数。
// =============================================================================
#pragma once
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#define HGC_SCHEDULER_ABI 1

namespace hgc {

// 模拟器向插件暴露的系统视图
class SystemView {
 public:
  virtual ~SystemView() = default;
  // 系统完整描述（设备推导峰值、存储、链路、拓扑），格式同前端“系统配置”页
  virtual nlohmann::json describe() const = 0;
  // 所有具备计算能力的设备 id（含主机 CPU）
  virtual std::vector<std::string> compute_devices() const = 0;
  virtual std::string device_type(const std::string& dev) const = 0;
  virtual std::string host_id() const = 0;
  virtual std::string local_memory(const std::string& dev) const = 0;  // 设备本地存储 id
  virtual double memory_capacity(const std::string& mem) const = 0;    // bytes
  // 整个阶段在该设备上独占运行（数据已在本地）的估计时间，秒；不支持返回 -1
  //   stage: feature_extraction | graph_construction | clustering
  //   precision: fp32 | fp16 | bf16 | fp8 | int8 | int4
  virtual double stage_time(const std::string& stage, const std::string& dev, const std::string& precision) const = 0;
  // 从存储 src 到节点 dst（设备或存储）的路径有效带宽 (bytes/s) 与延迟 (s)
  virtual double path_bandwidth(const std::string& src, const std::string& dst) const = 0;
  virtual double path_latency(const std::string& src, const std::string& dst) const = 0;
  // 路径是否支持访存语义（可零拷贝）
  virtual bool path_load_store(const std::string& src, const std::string& dst) const = 0;
};

// 负载视图
class WorkloadView {
 public:
  virtual ~WorkloadView() = default;
  virtual nlohmann::json describe() const = 0;          // dataset / algorithm 原始配置
  virtual nlohmann::json characterize() const = 0;      // 各阶段 FLOPs / 访存量 / 算术强度
  virtual std::string dataset_location() const = 0;     // 默认数据集所在设备
};

// 动态调度回调信息
struct TaskInfo {
  int id = 0;
  std::string name;
  std::string stage;
  int chunk = -1;
  int iter = -1;
  double flops = 0;
  double bytes = 0;
};

struct DeviceState {
  std::string id;
  std::string type;
  double now = 0;              // 当前仿真时间
  double busy_until = 0;       // 该设备预计空闲时间（含已排队任务）
  int queued = 0;              // 已排队任务数
  double est_compute_s = 0;    // 该任务在此设备上的估计计算时间
  double missing_input_bytes = 0;  // 尚未驻留在该设备本地的输入字节
  double input_bw = 0;         // 到该设备的近似输入带宽 (bytes/s)
};

class IScheduler {
 public:
  virtual ~IScheduler() = default;
  virtual std::string name() const = 0;
  // args 为策略 JSON 中的 plugin_args；base 为策略 JSON 自身（可作为默认值）
  virtual nlohmann::json plan(const SystemView& sys, const WorkloadView& wl, const nlohmann::json& args,
                              const nlohmann::json& base) = 0;
  // 返回 candidates 中的下标；返回 -1 表示使用模拟器内置的最早完成时间 (EFT) 策略
  virtual int select_device(const TaskInfo& task, const std::vector<DeviceState>& candidates) {
    (void)task;
    (void)candidates;
    return -1;
  }
};

}  // namespace hgc

#define HGC_EXPORT_SCHEDULER(CLASS)                                                         \
  extern "C" hgc::IScheduler* hgc_create_scheduler() { return new CLASS(); }                \
  extern "C" void hgc_destroy_scheduler(hgc::IScheduler* s) { delete s; }                   \
  extern "C" int hgc_scheduler_abi_version() { return HGC_SCHEDULER_ABI; }
