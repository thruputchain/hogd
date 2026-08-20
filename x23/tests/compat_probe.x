program CompatProbe {
  version CompatProbe1 {
    void ping(void) = 0;
  } = 1;

  version CompatProbe2 {
    void ping(void) = 0;
    void added(int) = 1;
  } = 2;
} = 0x7ee7aafe;
