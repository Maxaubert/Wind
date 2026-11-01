// vmouse: real HID pointer motion through Karabiner's virtual HID device (test harness for Wind).
// Run as root. Reads stdin lines: "move <dx> <dy> <count> <interval_ms>", "wheel <notches> <count> <interval_ms>",
// "type <text> [ms]" (a-z, 0-9, _ = space, < = backspace, > = tab), "button <n> down|up", "quit".
#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>
#include <cmath>
#include <pqrs/karabiner/driverkit/virtual_hid_device_driver.hpp>
#include <pqrs/karabiner/driverkit/virtual_hid_device_service.hpp>

namespace kd = pqrs::karabiner::driverkit;

int main() {
  pqrs::dispatcher::extra::initialize_shared_dispatcher();
  auto client = std::make_unique<kd::virtual_hid_device_service::client>();
  std::atomic<bool> ready(false), kbReady(false);
  client->connected.connect([&] {
    client->async_virtual_hid_pointing_initialize();
    kd::virtual_hid_device_service::virtual_hid_keyboard_parameters parameters;
    parameters.set_country_code(pqrs::hid::country_code::us);
    client->async_virtual_hid_keyboard_initialize(parameters);
  });
  client->virtual_hid_keyboard_ready.connect([&](auto&& r) { if (r) kbReady = true; });
  client->connect_failed.connect([](auto&& e) { std::cerr << "connect_failed " << e << std::endl; });
  client->virtual_hid_pointing_ready.connect([&](auto&& r) { if (r && !ready) { ready = true; std::cout << "ready" << std::endl; } });
  client->async_start();
  for (int i = 0; i < 100 && (!ready || !kbReady); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    } else if (cmd == "type") {
      // type <text>: lowercase letters, digits and '_' for space; real HID key presses
      std::string text; in >> text;
      int ms = 120; in >> ms;
      for (char ch : text) {
        uint16_t u = 0;
        if (ch >= 'a' && ch <= 'z') u = 0x04 + (ch - 'a');
        else if (ch >= '1' && ch <= '9') u = 0x1E + (ch - '1');
        else if (ch == '0') u = 0x27;
        else if (ch == '_') u = 0x2C;   // space
        else if (ch == '<') u = 0x2A;   // backspace
        else if (ch == '>') u = 0x2B;   // tab
        if (!u) continue;
        kd::virtual_hid_device_driver::hid_report::keyboard_input kr;
        kr.keys.insert(u);
        client->async_post_report(kr);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        client->async_post_report(kd::virtual_hid_device_driver::hid_report::keyboard_input());
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
    } else if (cmd == "circle") {
      // circle <radius counts per report> <reports> <interval_ms> <turns>: smooth round path,
      // fractional deltas carried so the integer reports trace the circle exactly
      double r = 1, turns = 1; int n = 100, ms = 1; in >> r >> n >> ms >> turns;
      double ax = 0, ay = 0;
      for (int i = 0; i < n; ++i) {
        double a = 2 * M_PI * turns * i / n;
        ax += r * cos(a); ay += r * sin(a);
        int ix = (int)lround(ax), iy = (int)lround(ay);
        ax -= ix; ay -= iy;
        report.x = static_cast<uint8_t>(static_cast<int8_t>(ix));
        report.y = static_cast<uint8_t>(static_cast<int8_t>(iy));
        client->async_post_report(report);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
      report.x = 0; report.y = 0;
      client->async_post_report(report);
    } else if (cmd == "ctrl") {
      // ctrl down|up: hold the left control key (for Ctrl+scroll)
      std::string st; in >> st;
      kd::virtual_hid_device_driver::hid_report::keyboard_input kr;
      if (st == "down") kr.modifiers.insert(kd::virtual_hid_device_driver::hid_report::modifier::left_control);
      client->async_post_report(kr);
    } else if (cmd == "wheel") {
      // wheel <notches> <count> <interval_ms>: one report per notch step
      int w = 1, n = 1, ms = 50; in >> w >> n >> ms;
      for (int i = 0; i < n; ++i) {
        report.vertical_wheel = static_cast<uint8_t>(static_cast<int8_t>(w));
        client->async_post_report(report);
        report.vertical_wheel = 0;
        client->async_post_report(report);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
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
  client->async_virtual_hid_keyboard_terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  client = nullptr;
  pqrs::dispatcher::extra::terminate_shared_dispatcher();
  return 0;
}
