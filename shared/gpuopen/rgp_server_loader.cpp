#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <string>
#include <thread>

#include "rdsServer.h"
#include "rgpHost.h"

using namespace DevDriver;

void runRgpHost() {
  fprintf(stderr, "Starting rgp_host\n");
  pthread_setname_np(pthread_self(), "RGP_HOST");
  RgpHost host;
  Result result = Result::Success;
  while (result == Result::Success) {
    result = host.UpdateState();
    usleep(100);
  }
  fprintf(stderr, "Terminated rgp_host\n");
}

int runRgpServer() {
  Result result;
  RdsServer server;

  result = server.Initialize();

  std::thread rgpHostThread(runRgpHost);
  pthread_setname_np(pthread_self(), "RGP_SERVER");

  while (result == Result::Success || result == Result::NotReady) {
    server.UpdateClients();
    result = server.ProcessMessage();
    // Could use better select logic with support for multiple sockets
    if (result == Result::NotReady) {
      usleep(100);
    }
  }
  fprintf(stderr, "Destroying rgp server\n");

  result = server.Destroy();
  rgpHostThread.join();
  return 0;
}

struct rgpServerRunner
{
  rgpServerRunner() {
    new std::thread(runRgpServer);
  }
} load_last;
