#include <unordered_map>

int main() {
  std::unordered_map<int, int> values;
  for (int i = 0; i < 50; ++i)
    values[i] = i * i;
  if (values.size() != 50 || values[7] != 49)
    return 1;
  values.erase(7);
  return values.count(7) == 0 && values[3] == 9 ? 0 : 2;
}
