#include <chrono>

int main() {
  using namespace std::chrono;
  auto span = 2s + 3s;
  if (span.count() != 5 || duration_cast<milliseconds>(span).count() != 5000)
    return 1;
  auto now = system_clock::now().time_since_epoch().count();
  return now > 0 ? 0 : 2;
}
