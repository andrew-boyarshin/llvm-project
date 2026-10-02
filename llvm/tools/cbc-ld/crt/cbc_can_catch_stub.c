/* Non-C++ definition of the type-matching hooks. A C++ link uses the
   strong definitions in libc++abi instead of this file. */
int __cbc_can_catch(const void *tinfo, void *exn, int sel) {
  (void)tinfo;
  (void)exn;
  (void)sel;
  return 0;
}

int __cbc_matches_filter(const void *tinfo, void *exn) {
  (void)tinfo;
  (void)exn;
  return 0;
}
