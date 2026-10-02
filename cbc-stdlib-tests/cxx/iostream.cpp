#include <cstdio>
#include <iostream>
#include <string>

int main() {
  const char *path = "/tmp/cbc-stdlib-cout.txt";
  if (!std::freopen(path, "w", stdout))
    return 1;
  std::cout << 1 << std::flush;
  std::fclose(stdout);
  FILE *in = std::fopen(path, "r");
  if (!in)
    return 2;
  char buf[8] = {};
  if (!std::fgets(buf, sizeof buf, in))
    return 3;
  std::fclose(in);
  std::remove(path);
  return std::string(buf) == "1" ? 0 : 4;
}
