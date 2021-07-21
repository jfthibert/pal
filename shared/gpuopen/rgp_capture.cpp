#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>

#include "rgp_capture_lib.h"

void print_usage(const char* binaryName) {
    fprintf(stderr, "Usage %s <hostname> <tracefile> [PreparationFrames] [--pipeline_hash hash]\n",
            binaryName);
}

int main(int argc, char* argv[]) {
  int numPreparationFrames = 2;
  uint64_t pipelineHash = 0;
  if (argc < 3) {
    print_usage(argv[0]);
    return 1;
  }
  if (argc > 3) {
    if (strcmp(argv[3], "--pipeline_hash") != 0) {
      numPreparationFrames = atoi(argv[3]);
    }

    for (int i = 3; i < argc; i++) {
      if (strcmp(argv[i], "--pipeline_hash") == 0) {
        if (i + 1 >= argc) {
          print_usage(argv[0]);
          return 1;
        }
        pipelineHash = std::stoull(argv[i+1], NULL, 16);
      }
    }

  }
  int status = CaptureRgp(argv[1], argv[2], numPreparationFrames, -1, pipelineHash);
  if (status != 0) {
    fprintf(stderr, "Capture failed with error code %d\n", status);
    return 1;
  }
  return 0;
}
