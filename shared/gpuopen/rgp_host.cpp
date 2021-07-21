#include <unistd.h>
#include <cstdio>

#include "rgpHost.h"

using namespace DevDriver;

int main(int argc, char* argv[]) {
  fprintf(stderr, "Starting rgp_host\n");
  RgpHost host;
  Result result = Result::Success;
  while (result == Result::Success) {
    result = host.UpdateState();
    usleep(100);
  }

  return 0;
}
