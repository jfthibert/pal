#ifndef RDPSERVER_H__
#define RDPSERVER_H__

#include <map>
#include <memory>

#include "gpuopen.h"
#include "protocols/systemProtocols.h"
#include "inc/ddAbstractSocket.h"

namespace {
class RdsClient;
}  // namespace

namespace DevDriver {
class RdsServer {
 public:
  RdsServer();
  Result Initialize();
  Result Destroy();
  Result ProcessMessage();
  void UpdateClients();

 private:
  Result ProcessClientMessage(const MessageBuffer& messageBuffer,
                              const void* pSockAddr, size_t addrSize,
                              SocketType socketType);
  Result SendMessage(ClientId srcClientId, ClientId dstClientId,
                     SystemProtocol::SystemMessage message,
                     const ClientMetadata& metadata);
  Result SendTo(const MessageBuffer& messageBuffer, std::string id);
  Result Send(const MessageBuffer& messageBuffer);
  ClientId getClientId(std::string id);
  ClientId createClient(std::string id);
  Result removeClient(std::string id);

  std::map<std::string, RdsClient*> m_clients;
  ClientId m_currentId;
  Socket m_serverSocket;
  Socket m_serverTcpSocket;
};
}  // namespace DevDriver

#endif  // RGPHOST_H__
