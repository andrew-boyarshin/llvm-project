#include <tuple>

int main() {
  auto values = std::make_tuple(1, 2, 'a');
  int a = 0;
  int b = 0;
  char c = 0;
  std::tie(a, b, c) = values;
  auto [x, y, z] = values;
  return a == 1 && b == 2 && c == 'a' && x == 1 && y == 2 && z == 'a' &&
                 std::get<0>(values) == 1
             ? 0
             : 1;
}
