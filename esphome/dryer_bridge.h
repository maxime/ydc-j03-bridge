#pragma once

#include <optional>
#include <string>

namespace yamazen {

void start();
void network_connected();
void press(const char* name);
void set_power(bool on);
void set_fan_speed(bool high);
void set_humidity(int percent);
void set_timer(int hours);
std::string display();
std::optional<bool> power();
std::optional<std::string> fan_speed();
float humidity();
float humidity_target();
float timer_hours();

}  // namespace yamazen
