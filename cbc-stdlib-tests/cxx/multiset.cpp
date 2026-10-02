#include <set>

int main() {
  std::multiset<int> values;
  values.insert(1);
  values.insert(1);
  values.insert(2);
  return values.size() == 3 && values.count(1) == 2 && *values.begin() == 1 ? 0
                                                                             : 1;
}
