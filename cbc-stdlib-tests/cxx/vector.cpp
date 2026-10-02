#include <vector>

int main() {
  std::vector<int> v;
  for (int i = 0; i < 100; ++i)
    v.push_back(i);
  if (v.size() != 100 || v.front() != 0 || v.back() != 99)
    return 1;
  v.erase(v.begin() + 50);
  if (v.size() != 99 || v[50] != 51)
    return 2;
  v.clear();
  return v.empty() ? 0 : 3;
}
