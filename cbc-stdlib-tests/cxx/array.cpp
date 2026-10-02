#include <array>

int main() {
  std::array<int, 3> values{1, 2, 3};
  return values.front() == 1 && values.back() == 3 && values.at(1) == 2 &&
                 values.size() == 3
             ? 0
             : 1;
}
