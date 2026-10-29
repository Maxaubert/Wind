// vmouse: real HID pointer motion through Karabiner's virtual HID device (test harness for Wind).
// Run as root. Reads stdin lines: "move <dx> <dy> <count> <interval_ms>", "button <n> down|up", "quit".
#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>
#include <pqrs/karabiner/driverkit/virtual_hid_device_driver.hpp>
#include <pqrs/karabiner/driverkit/virtual_hid_device_service.hpp>

namespace kd = pqrs::karabiner::driverkit;

int main() {
  pqrs::dispatcher::extra::initialize_shared_dispatcher();
  auto client = std::make_unique<kd::virtual_hid_device_service::client>();
  std::atomic<bool> ready(false);
  client->connected.connect([&] { client->async_virtual_hid_pointing_initialize(); });
  client->connect_failed.connect([](auto&& e) { std::cerr << "connect_failed " << e << std::endl; });
  client->virtual_hid_pointing_ready.connect([&](auto&& r) { if (r && !ready) { ready = true; std::cout << "ready" << std::endl; } });
  client->async_start();
  for (int i = 0; i < 100 && !ready; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (!ready) { std::cerr << "not ready" << std::endl; return 1; }
  kd::virtual_hid_device_driver::hid_report::pointing_input report;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "move") {
      int dx = 0, dy = 0, n = 1, ms = 8; in >> dx >> dy >> n >> ms;
      for (int i = 0; i < n; ++i) {
        report.x = static_cast<uint8_t>(static_cast<int8_t>(dx));
        report.y = static_cast<uint8_t>(static_cast<int8_t>(dy));
        client->async_post_report(report);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
      report.x = 0; report.y = 0;
      client->async_post_report(report);
    } else if (cmd == "button") {
      int b = 1; std::string st; in >> b >> st;
      if (st == "down") report.buttons.insert(b); else report.buttons.erase(b);
      report.x = 0; report.y = 0;
      client->async_post_report(report);
    } else if (cmd == "quit") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    std::cout << "ok" << std::endl;
  }
  client->async_virtual_hid_pointing_terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  client = nullptr;
  pqrs::dispatcher::extra::terminate_shared_dispatcher();
  return 0;
}
