#include <numeric>
#include <vector>

int main() {
  std::vector<int> values(5);
  std::iota(values.begin(), values.end(), 1);
  if (values.front() != 1 || values.back() != 5)
    return 1;
  int sum = std::accumulate(values.begin(), values.end(), 0);
  return sum == 15 ? 0 : 2;
}
