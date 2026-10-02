#include <algorithm>
#include <vector>

int main() {
  std::vector<int> values{4, 1, 3, 2};
  std::sort(values.begin(), values.end());
  if (values[0] != 1 || values[3] != 4)
    return 1;
  if (std::count(values.begin(), values.end(), 2) != 1)
    return 5;
  std::reverse(values.begin(), values.end());
  auto found = std::find(values.begin(), values.end(), 1);
  if (found == values.end() || *found != 1 || values.front() != 4)
    return 2;
  return std::min(4, 1) == 1 && std::max(4, 1) == 4 ? 0 : 3;
}
