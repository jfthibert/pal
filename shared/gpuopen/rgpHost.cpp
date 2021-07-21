#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "devDriverClient.h"
#include "msgChannel.h"
#include "protocols/driverControlClient.h"
#include "protocols/rgpClient.h"

#include "rgpHost.h"

using namespace DevDriver;

namespace {

void* DevDriverAlloc(void* pUserdata, size_t size, size_t alignment,
                     bool zero) {
  void* pMemory = aligned_alloc(alignment, size);
  if (pMemory && zero) {
    memset(pMemory, 0, size);
  }
  return pMemory;
}

void DevDriverFree(void* pUserdata, void* pMemory) { free(pMemory); }

void DevDriverBusEventCallback(void* pUserdata, BusEventType type,
                               const void* pEventData, size_t eventDataSize) {
    RgpHost* host = static_cast<RgpHost*>(pUserdata);
    host->processBusEvent(type, pEventData, eventDataSize);
}

}  // namespace

RgpHost::RgpHost()
    : m_DevDriverClient(),
      m_pDriverControlClient(),
      m_pRGPClient(),
      m_hostState(kHostUnitialized),
      m_rgpClientId() {}

Result RgpHost::UpdateState() {
  switch (m_hostState) {
    case kHostUnitialized: {
      DevDriver::ClientCreateInfo createInfo = {};
      createInfo.initialFlags =
          static_cast<StatusFlags>(ClientStatusFlags::DeveloperModeEnabled) |
          static_cast<StatusFlags>(ClientStatusFlags::DeviceHaltOnConnect);
      createInfo.connectionInfo = kDefaultNamedPipe;
      createInfo.componentType = Component::Tool;
      createInfo.createUpdateThread = true;
      std::snprintf(createInfo.clientDescription,
                    sizeof(createInfo.clientDescription), "RGP Host");
      DevDriver::AllocCb allocCb = {};
      allocCb.pUserdata = 0;
      allocCb.pfnAlloc = &DevDriverAlloc;
      allocCb.pfnFree = &DevDriverFree;

      m_DevDriverClient =
          std::make_unique<DevDriverClient>(allocCb, createInfo);
      m_hostState = kHostConnectingDev;
      break;
    }
    case kHostConnectingDev: {
      Result result = m_DevDriverClient->Initialize();
      if (result != Result::Success) {
        fprintf(stderr, "Initialize result %d\n", result);
        break;
      }
      if (m_DevDriverClient->IsConnected()) {
        IMsgChannel* pMsgChannel = m_DevDriverClient->GetMessageChannel();
        fprintf(stderr, "Connected as %d\n", pMsgChannel->GetClientId());
        BusEventCallback eventCallBack;
        eventCallBack.pfnEventCallback = DevDriverBusEventCallback;
        eventCallBack.pUserdata = this;
        pMsgChannel->SetBusEventCallback(eventCallBack);
        m_hostState = kHostWaitingClients;
      }
      break;
    }
    case kHostWaitingClients: {
      IMsgChannel* pMsgChannel = m_DevDriverClient->GetMessageChannel();
      MessageBuffer message;
      Result recv_result = pMsgChannel->Receive(message, 0);
      if (recv_result == Result::Success) {
        if (message.header.protocolId == Protocol::System) {
          switch (message.header.messageId) {
            case static_cast<MessageCode>(
                SystemProtocol::SystemMessage::Halted): {
              m_pDriverControlClient =
                  m_DevDriverClient
                      ->AcquireProtocolClient<Protocol::DriverControl>();
              m_pRGPClient =
                  m_DevDriverClient->AcquireProtocolClient<Protocol::RGP>();
              m_rgpClientId = message.header.srcClientId;
              m_hostState = kHostConnectingRgp;
              break;
            }
            case static_cast<MessageCode>(
                SystemProtocol::SystemMessage::ClientConnected): {
              ClientMetadata filter = {};
              pMsgChannel->Send(
                  message.header.srcClientId, Protocol::System,
                  static_cast<MessageCode>(
                      SystemProtocol::SystemMessage::QueryClientInfo),
                  filter, 0, NULL);
              break;
            }
            case static_cast<MessageCode>(
                SystemProtocol::SystemMessage::ClientDisconnected): {
              fprintf(stderr, "client %d disconnected\n",
                      message.header.srcClientId);
              break;
            }
            case static_cast<MessageCode>(
                SystemProtocol::SystemMessage::ClientInfo): {
              ClientInfoStruct clientInfo;
              if (sizeof(clientInfo) != message.header.payloadSize) {
                return Result::Error;
              }
              memcpy(&clientInfo, message.payload, sizeof(clientInfo));
              fprintf(stderr, "Client information for client id %d:\n",
                      message.header.srcClientId);
              fprintf(stderr, "%.128s %.128s process %d\n",
                      clientInfo.clientName, clientInfo.clientDescription,
                      clientInfo.processId);
              fprintf(stderr, "type %u status %u\n",
                      (unsigned int)clientInfo.metadata.clientType,
                      (unsigned int)clientInfo.metadata.status);
              break;
            }
            default:
              fprintf(stderr, "Unhandled system message\n");
          }
        }
      } else if (recv_result != Result::NotReady) {
        return Result::Error;
      }
      break;
    }
    case kHostConnectingRgp: {
      if (m_pRGPClient->IsConnected()) {
        Result result = m_pRGPClient->EnableProfiling();
        if (result != Result::Success) {
          fprintf(stderr, "Enabling profiling failed\n");
        } else {
          fprintf(stderr, "Profiling enabled on client %d\n", m_rgpClientId);
        }
        m_DevDriverClient->ReleaseProtocolClient(m_pRGPClient);
        m_pRGPClient = nullptr;
        m_hostState = kHostWaitingClients;
        if (m_pDriverControlClient->IsConnected()) {
          result = m_pDriverControlClient->ResumeDriver();
          if (result != Result::Success) {
            fprintf(stderr, "Resume driver failed\n");
          } else {
            fprintf(stderr, "driver resumed\n");
          }
          result = m_pDriverControlClient->WaitForDriverInitialization(10000);
          if (result != Result::Success) {
            fprintf(stderr, "Driver initialization wait failed\n");
          } else {
            fprintf(stderr, "Driver initialized\n");
          }
        }
        if (m_pDriverControlClient) {
          m_pDriverControlClient->Disconnect();
          m_DevDriverClient->ReleaseProtocolClient(m_pDriverControlClient);
          m_pDriverControlClient = nullptr;
        }
        if (m_pRGPClient) {
          m_pRGPClient->Disconnect();
          m_DevDriverClient->ReleaseProtocolClient(m_pRGPClient);
          m_pRGPClient = nullptr;
        }
        m_hostState = kHostWaitingClients;
      } else {
        m_pDriverControlClient->Connect(m_rgpClientId);
        m_pRGPClient->Connect(m_rgpClientId);
      }
      break;
    }
    default:
      break;
  }
  return Result::Success;
}

void RgpHost::processBusEvent(BusEventType type, const void* pEventData,
                         size_t eventDataSize) {
  if (type == BusEventType::ClientHalted) {
    const struct BusEventClientHalted* busEventClientHalted =
        static_cast<const struct BusEventClientHalted*>(pEventData);
    if (m_hostState == kHostWaitingClients) {
      m_pDriverControlClient =
          m_DevDriverClient->AcquireProtocolClient<Protocol::DriverControl>();
      m_pRGPClient =
          m_DevDriverClient->AcquireProtocolClient<Protocol::RGP>();
      m_rgpClientId = busEventClientHalted->clientId;
      m_hostState = kHostConnectingRgp;
    }
  }
}
