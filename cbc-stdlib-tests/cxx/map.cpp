#include <map>

int main() {
  std::map<int, int> values;
  values[2] = 20;
  values[1] = 10;
  if (values.size() != 2 || values.begin()->first != 1 || values[2] != 20)
    return 1;
  values.erase(2);
  return values.find(2) == values.end() && values[1] == 10 ? 0 : 2;
}
