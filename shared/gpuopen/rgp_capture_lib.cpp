#include <inttypes.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "devDriverClient.h"
#include "msgChannel.h"
#include "protocols/driverControlClient.h"
#include "protocols/rgpClient.h"

#include "rgp_capture_lib.h"

using namespace DevDriver;

struct ReadState {
  FILE* traceFile;
  uint32_t numChunks;
  uint64_t traceSizeInBytes;
  uint64_t bytesReceived;
};

namespace {

const int kConnectionDelayUs = 250 * 1000;
const int kMaxConnectRetries = 40;

#ifdef _WIN32
#include <malloc.h>
void* aligned_alloc(size_t alignment, size_t size) {
  return _aligned_malloc(size, alignment);
}
#endif
void* DevDriverAlloc(void* pUserdata, size_t size, size_t alignment,
                     bool zero) {
  void* pMemory = aligned_alloc(alignment, size);
  if (pMemory && zero) {
    memset(pMemory, 0, size);
  }
  return pMemory;
}

void DevDriverFree(void* pUserdata, void* pMemory) {
#ifdef _WIN32
  _aligned_free(pMemory);
#else
  free(pMemory);
#endif
}

void traceDataChunkReceived(const RGPProtocol::TraceDataChunk* pChunk,
                            void* pUserdata) {
  ReadState* readState = static_cast<ReadState*>(pUserdata);
  readState->bytesReceived +=
      fwrite(&pChunk->data[0], 1, pChunk->dataSize, readState->traceFile);
}

bool clientDiscoveredCallback(void* pUserdata, const DiscoveredClientInfo& clientInfo)
{
  std::atomic<ClientId>* driverClientId = static_cast<std::atomic<ClientId>*>(pUserdata);
  *driverClientId = clientInfo.id;
  return false;
}

}  // namespace

int CaptureRgp(char* hostname, char* filename, int preparationFrames,
               int clientId, uint64_t pipelineHash) {
  DevDriver::ClientCreateInfo createInfo = {};
  createInfo.initialFlags =
      static_cast<StatusFlags>(ClientStatusFlags::DeveloperModeEnabled);
  createInfo.componentType = Component::Tool;
  createInfo.createUpdateThread = true;
  std::snprintf(createInfo.clientDescription,
                sizeof(createInfo.clientDescription), "RGP Capture");
  createInfo.connectionInfo.type = TransportType::RemoteReliable;
  createInfo.connectionInfo.port = kDefaultNetworkPort;
  createInfo.connectionInfo.pHostname = hostname;
  DevDriver::AllocCb allocCb = {};
  allocCb.pUserdata = 0;
  allocCb.pfnAlloc = &DevDriverAlloc;
  allocCb.pfnFree = &DevDriverFree;

  auto pDevDriverClient =
      std::make_unique<DevDriverClient>(allocCb, createInfo);
  fprintf(stderr, "Waiting for connection\n");
  int retries = kMaxConnectRetries;
  while (!pDevDriverClient->IsConnected() && retries > 0) {
    Result result = pDevDriverClient->Initialize();
    if (result != Result::Success) {
      fprintf(stderr, "Initialize result %d\n", result);
    }
    fprintf(stderr, ".");
    std::this_thread::sleep_for(std::chrono::microseconds(kConnectionDelayUs));
    --retries;
  }

  if (retries == 0) {
    fprintf(stderr, "Couldn't connect to RDS\n");
    return 1;
  }

  fprintf(stderr, "\nConnected\n");
  IMsgChannel* pMsgChannel = pDevDriverClient->GetMessageChannel();

  std::atomic<ClientId> driverClientId;
  driverClientId = 0;

  if (clientId >= 0) {
    driverClientId = static_cast<ClientId>(clientId);
  } else {
    struct DiscoverClientsInfo discoverClientsInfo = {};
    discoverClientsInfo.pfnCallback = clientDiscoveredCallback;
    discoverClientsInfo.pUserdata = &driverClientId;
    discoverClientsInfo.filter.clientType = Component::Driver;
    discoverClientsInfo.timeoutInMs = kConnectionDelayUs * kMaxConnectRetries;
    Result result = pMsgChannel->DiscoverClients(discoverClientsInfo);
    if (result != Result::Success) {
      fprintf(stderr, "Couldn't find driver client\n");
      return 1;
    }
  }

  fprintf(stderr, "Trying to connect with driver control\n");
  DriverControlProtocol::DriverControlClient* pDriverControlClient;
  pDriverControlClient =
      pDevDriverClient->AcquireProtocolClient<Protocol::DriverControl>();
  pDriverControlClient->Connect(driverClientId);
  retries = kMaxConnectRetries;
  while (pDriverControlClient->IsConnected() != true && retries > 0) {
    fprintf(stderr, ".");
    std::this_thread::sleep_for(std::chrono::microseconds(kConnectionDelayUs));
    pDriverControlClient->Connect(driverClientId);
    --retries;
  }
  if (retries == 0) {
    fprintf(stderr, "Couldn't connect to driver\n");
    return 1;
  }

  Result result = pDriverControlClient->SetDeviceClockMode(
      0, DriverControlProtocol::DeviceClockMode::Profiling);
  if (result != Result::Success) {
    fprintf(stderr, "Warning, couldn't set profiling clock mode\n");
  }

  RGPProtocol::RGPClient* pRGPClient;
  pRGPClient = pDevDriverClient->AcquireProtocolClient<Protocol::RGP>();

  pRGPClient->Connect(driverClientId);
  retries = kMaxConnectRetries;
  while (pRGPClient->IsConnected() != true && retries > 0) {
    fprintf(stderr, ".");
    std::this_thread::sleep_for(std::chrono::microseconds(kConnectionDelayUs));
    pRGPClient->Connect(driverClientId);
    --retries;
  }
  if (retries == 0) {
    fprintf(stderr, "Couldn't connect to driver\n");
    return 1;
  }

  fprintf(stderr, "RGP connected\n");
  ReadState readState = {};

  RGPProtocol::ClientTraceParametersInfo paramInfo = {};
  paramInfo.gpuMemoryLimitInMb = 512;
  paramInfo.numPreparationFrames = preparationFrames;
  if (pipelineHash != 0) {
    paramInfo.pipelineHash = pipelineHash;
    paramInfo.flags.enableInstructionTokens = 1;
  }

  result = pRGPClient->UpdateTraceParameters(paramInfo);
  if (result != Result::Success) {
    fprintf(stderr, "UpdateTraceParameters failed (%d)\n", result);
    return 2;
  }

  RGPProtocol::BeginTraceInfo beginTrace = {};
  beginTrace.callbackInfo.chunkCallback = traceDataChunkReceived;
  beginTrace.callbackInfo.pUserdata = &readState;

  fprintf(stderr, "Beginning trace\n");
  result = pRGPClient->BeginTrace(beginTrace);
  if (result != Result::Success) {
    fprintf(stderr, "BeginTrace failed (%d)\n", result);
    return 2;
  }

  uint32 numChunks;
  uint64 traceSizeInBytes;
  constexpr auto kTimeout = 5u * 60u * 1000u;  // 5 min timeout
  result = pRGPClient->EndTrace(&numChunks, &traceSizeInBytes, kTimeout);
  if (result != Result::Success) {
    fprintf(stderr, "EndTrace failed (%d)\n", result);
    return 3;
  }

  result = pDriverControlClient->SetDeviceClockMode(
      0, DriverControlProtocol::DeviceClockMode::Default);
  if (result != Result::Success) {
    fprintf(stderr, "Warning, couldn't restore default clock mode\n");
  }

  fprintf(stderr, "Trace in %u chunks and total size of %" PRIu64 "\n",
          numChunks, traceSizeInBytes);
  readState.traceFile = fopen(filename, "wb");
  if (readState.traceFile == nullptr) {
    fprintf(stderr, "Couldn't open rgp file %s (%s)\n", filename, strerror(errno));
    return 5;
  }
  readState.numChunks = numChunks;
  readState.traceSizeInBytes = traceSizeInBytes;

  while ((result = pRGPClient->ReadTraceDataChunk()) == Result::Success) {
  }
  fclose(readState.traceFile);

  if (readState.traceSizeInBytes != readState.bytesReceived) {
    fprintf(stderr, "Expected %" PRIu64 " bytes got %" PRIu64 " bytes\n",
            readState.traceSizeInBytes, readState.bytesReceived);
    return 4;
  }
  return 0;
}
