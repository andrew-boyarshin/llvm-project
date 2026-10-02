#include <string_view>

int main() {
  std::string_view view = "abcdef";
  view.remove_prefix(2);
  return view == "cdef" && view.size() == 4 ? 0 : 1;
}
