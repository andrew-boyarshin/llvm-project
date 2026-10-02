int div_quot(int n, int d) { return n / d; }
int div_rem(int n, int d) { return n % d; }
unsigned udiv_q(unsigned n, unsigned d) { return n / d; }
unsigned urem_r(unsigned n, unsigned d) { return n % d; }

int main(void) {
  if (div_quot(20, 3) != 6 || div_rem(20, 3) != 2)
    return 1;
  if (div_quot(-20, 3) != -6 || div_rem(-20, 3) != -2)
    return 2;
  if (udiv_q(20u, 3u) != 6u || urem_r(20u, 3u) != 2u)
    return 3;
  if (udiv_q((unsigned)-1, 2u) != 2147483647u)
    return 4;
  return 0;
}
