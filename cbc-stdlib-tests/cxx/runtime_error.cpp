#include <stdexcept>
#include <string>

int main() {
  try {
    throw std::runtime_error("boom");
  } catch (const std::exception &error) {
    return std::string(error.what()) == "boom" ? 0 : 1;
  } catch (...) {
    return 2;
  }
  return 3;
}
