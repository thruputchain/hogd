#include "hogd/hog_exec/test_main.h"

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  return hogd::run_hog_exec_tests(argc, argv);
}
