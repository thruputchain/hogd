#include "hogd/base/log.h"
#include "hogd/init.h"

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("hello from hogexec!");
}
