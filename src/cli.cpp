// hgcsim_cli : 命令行入口（不依赖 Python）
//   hgcsim_cli catalog
//   hgcsim_cli validate <system.json>
//   hgcsim_cli characterize <workload.json>
//   hgcsim_cli run <system.json> <workload.json> <strategy.json> [out.json]
#include <fstream>
#include <iostream>
#include <sstream>

#include "hgcsim/runtime.hpp"
#include "hgcsim/system.hpp"

using hgc::json;

static json load(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("无法打开文件 " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return json::parse(ss.str());
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "用法:\n  hgcsim_cli catalog\n  hgcsim_cli validate <system.json>\n"
                 "  hgcsim_cli characterize <workload.json>\n"
                 "  hgcsim_cli run <system.json> <workload.json> <strategy.json> [out.json]\n";
    return 2;
  }
  try {
    std::string cmd = argv[1];
    if (cmd == "catalog") {
      std::cout << hgc::catalog().dump(2) << "\n";
    } else if (cmd == "validate" && argc >= 3) {
      json r = hgc::validate_system(load(argv[2]));
      std::cout << r.dump(2) << "\n";
      return r.value("ok", false) ? 0 : 1;
    } else if (cmd == "characterize" && argc >= 3) {
      std::cout << hgc::characterize(load(argv[2])).dump(2) << "\n";
    } else if (cmd == "run" && argc >= 5) {
      json req = {{"system", load(argv[2])}, {"workload", load(argv[3])}, {"strategy", load(argv[4])}};
      json r = hgc::simulate(req);
      if (argc >= 6) {
        std::ofstream o(argv[5]);
        o << r.dump(1);
      }
      if (r.value("status", "") == "ok") {
        std::cout << "status      : ok\n";
        std::cout << "makespan    : " << r["makespan_s"].get<double>() << " s (仿真时间)\n";
        std::cout << "wall time   : " << r["wall_time_s"].get<double>() << " s\n";
        std::cout << "chunks      : " << r["graph"]["chunks"] << "  tasks: " << r["graph"]["tasks"] << "\n";
        for (auto it = r["stages"].begin(); it != r["stages"].end(); ++it)
          std::cout << "  stage " << it.key() << ": " << it.value()["start_s"].get<double>() << " -> "
                    << it.value()["end_s"].get<double>() << " s\n";
        std::cout << "energy      : " << r["energy"]["total_j"].get<double>() << " J\n";
        for (auto& w : r["warnings"]) std::cout << "  [warn] " << w.get<std::string>() << "\n";
      } else {
        std::cout << "status: error\n";
        for (auto& e : r["errors"]) std::cout << "  " << e.get<std::string>() << "\n";
        return 1;
      }
    } else {
      std::cerr << "未知命令或参数不足\n";
      return 2;
    }
  } catch (std::exception& ex) {
    std::cerr << "错误: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}
