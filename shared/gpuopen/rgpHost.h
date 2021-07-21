#ifndef RGPHOST_H__
#define RGPHOST_H__

#include <memory>

#include "devDriverClient.h"
#include "gpuopen.h"

namespace DevDriver {
namespace RGPProtocol {
class RGPClient;
}  // namespace RGPProtocol

class RgpHost {
  enum RgpHostState {
    kHostUnitialized,
    kHostConnectingDev,
    kHostWaitingClients,
    kHostConnectingRgp,
  };

 public:
  RgpHost();
  Result UpdateState();
  void processBusEvent(BusEventType type, const void* pEventData,
                       size_t eventDataSize);

 private:
  std::unique_ptr<DevDriverClient> m_DevDriverClient;
  DriverControlProtocol::DriverControlClient* m_pDriverControlClient;
  RGPProtocol::RGPClient* m_pRGPClient;
  RgpHostState m_hostState;
  ClientId m_rgpClientId;
};
}  // namespace DevDriver

#endif  // RGPHOST_H__
