#include <algorithm>
#include <utility>

int main() {
  auto p = std::make_pair(1, 2);
  std::swap(p.first, p.second);
  return p.first == 2 && p.second == 1 && std::min(3, 1) == 1 &&
                 std::max(3, 1) == 3
             ? 0
             : 1;
}
