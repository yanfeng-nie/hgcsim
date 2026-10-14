/*
 * c_api.h : HGCSim 的 C ABI —— Python 前端通过 ctypes 调用
 * 所有函数输入/输出均为 UTF-8 JSON 字符串；返回的字符串需用 hgc_free 释放。
 */
#ifndef HGCSIM_C_API_H
#define HGCSIM_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

const char* hgc_version(void);
/* 当前 build 信息：{"name", "time", "modified", "plugins", "reserved_names", ...} */
char* hgc_build_info(void);
/* 所有组件类型的参数说明（默认值 / 上下界 / 单位） */
char* hgc_catalog(void);
/* 校验系统配置并返回推导指标 */
char* hgc_validate_system(const char* system_json);
/* 负载特征分析（各阶段 FLOPs / 访存量 / 算术强度） */
char* hgc_characterize(const char* workload_json);
/* 仅运行规划（插件 plan / 自动分块），返回最终策略 */
char* hgc_plan(const char* request_json);
/* 运行一次仿真：{"system":..,"workload":..,"strategy":..,"options":..} */
char* hgc_simulate(const char* request_json);
void hgc_free(char* p);

#ifdef __cplusplus
}
#endif
#endif
