#include "rdsServer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "ddPlatform.h"
#include "devDriverClient.h"
#include "protocols/driverControlClient.h"
#include "protocols/rgpClient.h"
#include "rgpHost.h"

using namespace DevDriver;

//#define LOG_MESSAGES

namespace {

class RdsClient {
 public:
  RdsClient(ClientId clientId)
      : m_clientId(clientId),
        m_flags(),
        m_componentType(),
        m_clientTcpSocket(nullptr) {}

  ClientId m_clientId;
  StatusFlags m_flags;
  Component m_componentType;
  std::unique_ptr<Socket> m_clientTcpSocket;
  bool m_remove = false;
};

}  // namespace

RdsServer::RdsServer() : m_currentId(1), m_serverSocket() {}

Result RdsServer::Initialize() {
  Result result = Result::Error;

  result = m_serverSocket.Init(true, SocketType::Local);

  if (result == Result::Success) {
    result = m_serverSocket.Bind("AMD-Developer-Service", 0);
  }

  if (result == Result::Success) {
    result = m_serverTcpSocket.Init(false, SocketType::Tcp);
  }

  if (result == Result::Success) {
    result = m_serverTcpSocket.Bind("0.0.0.0", kDefaultNetworkPort);
  }

  if (result == Result::Success) {
    result = m_serverTcpSocket.Listen(16);
  }

  return result;
}

Result RdsServer::Destroy() {
  Result result = Result::Error;
  return result;
}

Result RdsServer::ProcessMessage() {
  Result result = Result::Success;
  bool canRead = true;
  bool exceptState = false;
  MessageBuffer messageBuffer;
  size_t bytesReceived;

  Result local_result =
      m_serverSocket.Select(&canRead, nullptr, &exceptState, 0);

  if (result == Result::Success) {
    if (canRead) {
      sockaddr_un addr = {};
      size_t addr_len = sizeof(addr);
      local_result = m_serverSocket.ReceiveFrom(
          &addr, &addr_len, reinterpret_cast<uint8*>(&messageBuffer),
          sizeof(MessageBuffer), &bytesReceived);

      if (bytesReceived !=
          (sizeof(messageBuffer.header) + messageBuffer.header.payloadSize)) {
        return Result::Rejected;
      }

      if (local_result == Result::Success) {
#ifdef LOG_MESSAGES
        if (!((messageBuffer.header.protocolId == Protocol::System &&
               (messageBuffer.header.messageId ==
                    static_cast<MessageCode>(
                        SystemProtocol::SystemMessage::Ping) ||
                messageBuffer.header.messageId ==
                    static_cast<MessageCode>(
                        SystemProtocol::SystemMessage::Pong))) ||
              (messageBuffer.header.protocolId == Protocol::ClientManagement &&
               messageBuffer.header.messageId ==
                   static_cast<MessageCode>(
                       ClientManagementProtocol::ManagementMessage::
                           KeepAlive)))) {
          fprintf(stderr,
                  "Received packet from %.107s addr len %ld data len %ld\n",
                  &addr.sun_path[1], addr_len, bytesReceived);
          fprintf(stderr, "Received from %d to %d proto %u msg %d\n",
                  messageBuffer.header.srcClientId,
                  messageBuffer.header.dstClientId,
                  (unsigned int)messageBuffer.header.protocolId,
                  messageBuffer.header.messageId);
        }
#endif  // LOG_MESSAGES
      }

      if (messageBuffer.header.protocolId == Protocol::ClientManagement) {
        ProcessClientMessage(messageBuffer, &addr, addr_len, SocketType::Local);
      } else {
        for (auto& client : m_clients) {
          if (client.second->m_remove) {
              continue;
          }
          if ((messageBuffer.header.dstClientId == 0 &&
               client.second->m_clientId != messageBuffer.header.srcClientId) ||
              messageBuffer.header.dstClientId == client.second->m_clientId) {
            result = SendTo(messageBuffer, client.first);
            if (result != Result::Success) {
              client.second->m_remove = true;
              result = Result::Success;
            }
          }
        }
      }
    } else if (exceptState) {
      local_result = Result::Error;
    } else {
      local_result = Result::NotReady;
    }
  }

  Result remote_result =
      m_serverTcpSocket.Select(&canRead, nullptr, &exceptState, 0);
  {
    if (canRead) {
      std::unique_ptr<Socket> clientSocket = std::make_unique<Socket>();
      m_serverTcpSocket.Accept(clientSocket.get());
      char addressName[128];
      std::string id = "tcp";
      snprintf(addressName, sizeof(addressName), ":%p", clientSocket.get());
      id.append(addressName, strlen(addressName));
      createClient(id);
      m_clients[id]->m_clientTcpSocket = std::move(clientSocket);
    } else if (exceptState) {
      remote_result = Result::Error;
    } else {
      remote_result = Result::NotReady;
    }
  }

  if (local_result == Result::NotReady && remote_result == Result::NotReady) {
    result = Result::NotReady;
  }

  // Go through all TCP sockets and check if they have data ready
  for (auto& client : m_clients) {
    if (client.second->m_remove) {
        continue;
    }
    if (client.second->m_clientTcpSocket != nullptr) {
      Result client_result = client.second->m_clientTcpSocket->Select(
          &canRead, nullptr, &exceptState, 0);
      if (client_result == Result::Success) {
        if (canRead) {
          local_result = client.second->m_clientTcpSocket->Receive(
              reinterpret_cast<uint8*>(&messageBuffer), sizeof(MessageBuffer),
              &bytesReceived);

          if (local_result == Result::Success) {
            if (bytesReceived != (sizeof(messageBuffer.header) +
                                  messageBuffer.header.payloadSize)) {
              fprintf(stderr, "dropping rejected tcp packet\n");
              return Result::Rejected;
            }
#ifdef LOG_MESSAGES
            if (!((messageBuffer.header.protocolId == Protocol::System &&
                   (messageBuffer.header.messageId ==
                        static_cast<MessageCode>(
                            SystemProtocol::SystemMessage::Ping) ||
                    messageBuffer.header.messageId ==
                        static_cast<MessageCode>(
                            SystemProtocol::SystemMessage::Pong))) ||
                  (messageBuffer.header.protocolId ==
                       Protocol::ClientManagement &&
                   messageBuffer.header.messageId ==
                       static_cast<MessageCode>(
                           ClientManagementProtocol::ManagementMessage::
                               KeepAlive)))) {
              fprintf(stderr, "Received packet from client %d data len %ld\n",
                      client.second->m_clientId, bytesReceived);
              fprintf(stderr, "Received from %d to %d proto %u msg %d\n",
                      messageBuffer.header.srcClientId,
                      messageBuffer.header.dstClientId,
                      (unsigned int)messageBuffer.header.protocolId,
                      messageBuffer.header.messageId);
            }
#endif  // LOG_MESSAGES
            if (messageBuffer.header.protocolId == Protocol::ClientManagement) {
              char addressName[128];
              snprintf(addressName, sizeof(addressName), "tcp:%p",
                       client.second->m_clientTcpSocket.get());
              ProcessClientMessage(messageBuffer, addressName,
                                   strlen(addressName), SocketType::Tcp);
            } else {
              for (auto& client : m_clients) {
                if (client.second->m_remove) {
                    continue;
                }
                if ((messageBuffer.header.dstClientId == 0 &&
                     client.second->m_clientId !=
                         messageBuffer.header.srcClientId) ||
                    messageBuffer.header.dstClientId ==
                        client.second->m_clientId) {
                  result = SendTo(messageBuffer, client.first);
                  if (result != Result::Success) {
                    client.second->m_remove = true;
                    result = Result::Success;
                  }
                }
              }
            }
          } else if (local_result != Result::NotReady) {
            client.second->m_remove = true;
            result = Result::Success;
            break;
          }
        } else if (exceptState) {
          client_result = Result::Error;
        } else {
          local_result = Result::NotReady;
        }
        if (result == Result::NotReady && local_result != Result::NotReady) {
          result = local_result;
        }
      }
    }
  }
  for (auto client = m_clients.begin(), next_client = client;
      client != m_clients.end(); client = next_client) {
    ++next_client;
    if (client->second->m_remove) {
      removeClient(client->first);
    }
  }
  return result;
}

// handle removal of clients on timeout
void RdsServer::UpdateClients() {
  // TODO timeouts
}

Result RdsServer::ProcessClientMessage(const MessageBuffer& messageBuffer,
                                       const void* pSockAddr, size_t addrSize,
                                       SocketType socketType) {
  Result result = Result::Success;
  if (ClientManagementProtocol::IsOutOfBandMessage(messageBuffer) &
      ClientManagementProtocol::IsValidOutOfBandMessage(messageBuffer)) {
    switch (static_cast<ClientManagementProtocol::ManagementMessage>(
        messageBuffer.header.messageId)) {
      case ClientManagementProtocol::ManagementMessage::ConnectRequest: {
        const ClientManagementProtocol::ConnectRequestPayload* pConnectRequest =
            reinterpret_cast<
                const ClientManagementProtocol::ConnectRequestPayload*>(
                &messageBuffer.payload[0]);
        MessageBuffer response = ClientManagementProtocol::kOutOfBandMessage;
        response.header.messageId = static_cast<MessageCode>(
            ClientManagementProtocol::ManagementMessage::ConnectResponse);
        response.header.payloadSize =
            sizeof(ClientManagementProtocol::ConnectResponsePayload);
        ClientManagementProtocol::ConnectResponsePayload* pConnectResponse =
            reinterpret_cast<ClientManagementProtocol::ConnectResponsePayload*>(
                &response.payload[0]);
        pConnectResponse->result = Result::Success;

        std::string id;
        id.assign(reinterpret_cast<const char*>(pSockAddr), addrSize);
        if (socketType == SocketType::Tcp) {
          pConnectResponse->clientId = m_clients[id]->m_clientId;
        } else {
          pConnectResponse->clientId = createClient(id);
        }

        m_clients[id]->m_flags = pConnectRequest->initialClientFlags;
        m_clients[id]->m_componentType = pConnectRequest->componentType;

        result = SendTo(response, id);
        if (result != Result::Success) {
          m_clients[id]->m_remove = true;
          result = Result::Success;
        }

        // Let other client know about the new connection
        ClientMetadata filter = {};
        SendMessage(pConnectResponse->clientId, kBroadcastClientId,
                    SystemProtocol::SystemMessage::ClientConnected, filter);

        break;
      }
      case ClientManagementProtocol::ManagementMessage::DisconnectNotification:
        // Remove the client
        result = Result::Error;
        DD_ASSERT_ALWAYS();
        break;
      case ClientManagementProtocol::ManagementMessage::SetClientFlags:
        // Update the flags
        result = Result::Error;
        DD_ASSERT_ALWAYS();
        break;
      case ClientManagementProtocol::ManagementMessage::QueryStatus: {
        // Assume DeveloperMode when we are running
        StatusFlags flags =
            static_cast<StatusFlags>(ClientStatusFlags::DeveloperModeEnabled);
        // Iterate over client and add their flags
        for (auto& client : m_clients) {
          flags |= client.second->m_flags;
        }

        MessageBuffer response = ClientManagementProtocol::kOutOfBandMessage;
        response.header.messageId = static_cast<MessageCode>(
            ClientManagementProtocol::ManagementMessage::QueryStatusResponse);
        response.header.payloadSize =
            sizeof(ClientManagementProtocol::QueryStatusResponsePayload);
        ClientManagementProtocol::QueryStatusResponsePayload*
            pQueryStatusResponse = reinterpret_cast<
                ClientManagementProtocol::QueryStatusResponsePayload*>(
                &response.payload[0]);
        pQueryStatusResponse->result = Result::Success;
        pQueryStatusResponse->flags = flags;
        std::string id;
        id.assign(reinterpret_cast<const char*>(pSockAddr), addrSize);
        result = SendTo(response, id);
        if (result != Result::Success) {
          m_clients[id]->m_remove = true;
          result = Result::Success;
        }
        break;
      }
      case ClientManagementProtocol::ManagementMessage::KeepAlive: {
        MessageBuffer keepAliveResponse =
            ClientManagementProtocol::kOutOfBandMessage;
        keepAliveResponse.header.messageId = static_cast<MessageCode>(
            ClientManagementProtocol::ManagementMessage::KeepAlive);
        keepAliveResponse.header.payloadSize = 0;
        keepAliveResponse.header.sessionId = messageBuffer.header.sessionId;
        std::string id;
        id.assign(reinterpret_cast<const char*>(pSockAddr), addrSize);
        result = SendTo(keepAliveResponse, id);
        if (result != Result::Success) {
          m_clients[id]->m_remove = true;
          result = Result::Success;
        }
        break;
      }
      default:
        fprintf(stderr, "Unexpected ManagementMessage %d\n",
                messageBuffer.header.messageId);
        result = Result::Error;
        DD_ASSERT_ALWAYS();
        break;
    }
  }
  return result;
}

// Pack a ClientMetadata into a 64-bit sequence number.
static uint64 MetadataToUint64(const ClientMetadata& metadata) {
  static_assert(sizeof(uint64) == sizeof(ClientMetadata),
                "Size of ClientMetadata is no longer 64-bits; "
                "MetadataToUint64() is invalid");
  // Bits 0-31
  uint64 ret = metadata.protocols.value;
  // Bits 32-39
  ret |= static_cast<uint64>(metadata.clientType) << 32;
  // metadata.reserved is bits 40-47
  // Bits 48-63
  ret |= static_cast<uint64>(metadata.status) << 48;
  return ret;
}

Result RdsServer::SendMessage(ClientId srcClientId, ClientId dstClientId,
                              SystemProtocol::SystemMessage message,
                              const ClientMetadata& metadata) {
#ifdef LOG_MESSAGES
  if (message != SystemProtocol::SystemMessage::Ping &&
      message != SystemProtocol::SystemMessage::Pong) {
    fprintf(stderr, "SendMessage %d from %d to %d\n", (int)message, srcClientId,
            dstClientId);
  }
#endif  // LOG_MESSAGES
  MessageBuffer messageBuffer = {};
  messageBuffer.header.srcClientId = srcClientId;
  messageBuffer.header.dstClientId = dstClientId;
  messageBuffer.header.protocolId = Protocol::System;
  messageBuffer.header.messageId = static_cast<MessageCode>(message);
  messageBuffer.header.sequence = MetadataToUint64(metadata);
  return Send(messageBuffer);
}

Result RdsServer::SendTo(const MessageBuffer& messageBuffer, std::string id) {
  Result result = Result::Error;
  const void* pSockAddr = id.data();
  size_t addrSize = id.size();
  size_t bytesWritten = 0;

#ifdef LOG_MESSAGES
  if (!((messageBuffer.header.protocolId == Protocol::System &&
         (messageBuffer.header.messageId ==
              static_cast<MessageCode>(SystemProtocol::SystemMessage::Ping) ||
          messageBuffer.header.messageId ==
              static_cast<MessageCode>(SystemProtocol::SystemMessage::Pong))) ||
        (messageBuffer.header.protocolId == Protocol::ClientManagement &&
         messageBuffer.header.messageId ==
             static_cast<MessageCode>(
                 ClientManagementProtocol::ManagementMessage::KeepAlive)))) {
    fprintf(stderr, "Sending OOB proto %u msg %d to %s\n",
            (unsigned int)messageBuffer.header.protocolId,
            messageBuffer.header.messageId, &((char*)pSockAddr)[3]);
  }
#endif  // LOG_MESSAGES

  size_t messageLength =
      sizeof(messageBuffer.header) + messageBuffer.header.payloadSize;
  if (m_clients.count(id) > 0 && m_clients[id]->m_clientTcpSocket != nullptr) {
    result = m_clients[id]->m_clientTcpSocket->Send(
        reinterpret_cast<const uint8*>(&messageBuffer), messageLength,
        &bytesWritten);
  } else {
    result = m_serverSocket.SendTo(
        pSockAddr, addrSize, reinterpret_cast<const uint8*>(&messageBuffer),
        messageLength, &bytesWritten);
  }
  if (result == Result::Success && bytesWritten != messageLength) {
    result = Result::Error;
  }

  return result;
}

Result RdsServer::Send(const MessageBuffer& messageBuffer) {
  Result result = Result::Error;
  for (auto& client : m_clients) {
    if (client.second->m_remove) {
        continue;
    }
    if ((messageBuffer.header.dstClientId == 0 &&
         client.second->m_clientId != messageBuffer.header.srcClientId) ||
        messageBuffer.header.dstClientId == client.second->m_clientId) {
      const void* pSockAddr = client.first.data();
      size_t addrSize = client.first.size();
      size_t bytesWritten = 0;

#ifdef LOG_MESSAGES
      if (!((messageBuffer.header.protocolId == Protocol::System &&
             (messageBuffer.header.messageId ==
                  static_cast<MessageCode>(
                      SystemProtocol::SystemMessage::Ping) ||
              messageBuffer.header.messageId ==
                  static_cast<MessageCode>(
                      SystemProtocol::SystemMessage::Pong))) ||
            (messageBuffer.header.protocolId == Protocol::ClientManagement &&
             messageBuffer.header.messageId ==
                 static_cast<MessageCode>(ClientManagementProtocol::
                                              ManagementMessage::KeepAlive)))) {
        fprintf(stderr, "Sending from %d to %d (%s) proto %u msg %d\n",
                messageBuffer.header.srcClientId,
                messageBuffer.header.dstClientId, &((char*)pSockAddr)[3],
                (unsigned int)messageBuffer.header.protocolId,
                messageBuffer.header.messageId);
      }
#endif  // LOG_MESSAGES

      size_t messageLength =
          sizeof(messageBuffer.header) + messageBuffer.header.payloadSize;
      if (client.second->m_clientTcpSocket != nullptr) {
        result = client.second->m_clientTcpSocket->Send(
            reinterpret_cast<const uint8*>(&messageBuffer), messageLength,
            &bytesWritten);
      } else {
        result = m_serverSocket.SendTo(
            pSockAddr, addrSize, reinterpret_cast<const uint8*>(&messageBuffer),
            messageLength, &bytesWritten);
      }

      if (result != Result::Success) {
        client.second->m_remove = true;
        result = Result::Success;
      }
      if (result == Result::Success && bytesWritten != messageLength) {
        result = Result::Error;
        break;
      }
    }
  }
  return result;
}

ClientId RdsServer::getClientId(std::string id) {
  ClientId clientId = 0;
  if (m_clients.count(id) > 0) {
    clientId = m_clients[id]->m_clientId;
  }
  return clientId;
}

ClientId RdsServer::createClient(std::string id) {
  fprintf(stderr, "Adding client %s\n", &(id.c_str()[3]));
  m_clients[id] = new RdsClient(m_currentId);
  DD_ASSERT(m_currentId < 65535);
  ++m_currentId;
  return m_clients[id]->m_clientId;
}

Result RdsServer::removeClient(std::string id) {
  Result result = Result::Error;
  if (m_clients.count(id) > 0) {
    fprintf(stderr, "Removing client %s\n", &(id.c_str()[3]));
    ClientId disconnectedId = m_clients[id]->m_clientId;
    delete m_clients[id];
    m_clients.erase(id);
    ClientMetadata filter = {};
    SendMessage(disconnectedId, kBroadcastClientId,
                SystemProtocol::SystemMessage::ClientDisconnected, filter);
    result = Result::Success;
  }
  return result;
}
