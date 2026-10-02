#include <complex>
#include <cmath>

int main() {
  std::complex<double> a(3, 4);
  std::complex<double> b = a + std::complex<double>(1, -4);
  if (std::lround(b.real()) != 4)
    return 1;
  if (std::lround(b.imag()) != 0)
    return 2;
  if (std::lround(std::norm(a)) != 25)
    return 3;
  return 0;
}
