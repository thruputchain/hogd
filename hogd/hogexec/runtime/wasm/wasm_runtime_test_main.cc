#include "hogd/hogexec/test_main.h"

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  return hogd::run_hogexec_tests(argc, argv);
}
