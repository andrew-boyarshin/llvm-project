#include <string>

int main() {
  std::string s = "hello";
  s += " world";
  if (s.find("world") != 6)
    return 1;
  if (s.substr(6) != "world")
    return 2;
  if (std::stof("1.5") < 1.4 || std::stof("1.5") > 1.6)
    return 3;
  return s.size() == 11 ? 0 : 4;
}
