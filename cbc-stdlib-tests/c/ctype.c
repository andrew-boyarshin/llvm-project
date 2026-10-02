#include <ctype.h>

static int digit_ok(void) {
  if (!isdigit((unsigned char)'7'))
    return 0;
  if (isdigit((unsigned char)'a'))
    return 0;
  return 1;
}

static int alpha_space_ok(void) {
  if (!isalpha((unsigned char)'Z'))
    return 0;
  if (!isspace((unsigned char)'\n'))
    return 0;
  return 1;
}

static int case_ok(void) {
  if (toupper((unsigned char)'b') != 'B')
    return 0;
  if (tolower((unsigned char)'Q') != 'q')
    return 0;
  return 1;
}

static int hex_ok(void) {
  if (!isxdigit((unsigned char)'f'))
    return 0;
  if (isxdigit((unsigned char)'g'))
    return 0;
  return 1;
}

int main(void) {
  if (!digit_ok())
    return 1;
  if (!alpha_space_ok())
    return 2;
  if (!case_ok())
    return 3;
  if (!hex_ok())
    return 4;
  return 0;
}
