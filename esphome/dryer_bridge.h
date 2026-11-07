#pragma once

#include <optional>
#include <string>

namespace yamazen {

void start();
void network_connected();
void press(const char* name);
void set_power(bool on);
void set_humidity(int percent);
void set_timer(int hours);
std::string display();
std::optional<bool> power();
float humidity();

}  // namespace yamazen
