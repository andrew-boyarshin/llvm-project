#include <variant>

int main() {
  std::variant<int, char> value = 3;
  if (!std::holds_alternative<int>(value) || std::get<int>(value) != 3)
    return 1;
  value = 'a';
  return std::holds_alternative<char>(value) && std::get<char>(value) == 'a' ? 0
                                                                              : 2;
}
