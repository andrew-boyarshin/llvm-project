#include <set>

int main() {
  std::set<int> values;
  values.insert(3);
  values.insert(1);
  values.insert(3);
  if (values.size() != 2 || *values.begin() != 1 || values.count(3) != 1)
    return 1;
  values.erase(1);
  return values.find(1) == values.end() && values.count(3) == 1 ? 0 : 2;
}
