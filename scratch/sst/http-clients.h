/* http-clients.h */

#ifndef HTTP_CLIENTS_H
#define HTTP_CLIENTS_H

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "web-workload.h"
#include "sst-protocol.h"
#include <vector>
#include <map>

namespace ns3 {

/**
 * Base class for HTTP client applications
 */
class HttpClientBase : public Application
{
public:
  static TypeId GetTypeId(void);
  HttpClientBase();
  virtual ~HttpClientBase();
  
  void SetWorkload(const WebWorkload* workload);
  void SetServerAddress(Address serverAddress);
  
protected:
  virtual void DoDispose(void);
  virtual void StartApplication(void);
  virtual void StopApplication(void);
  
  // Methods to be implemented by subclasses
  virtual void ProcessNextPage() = 0;
  
  Address m_serverAddress;
  const WebWorkload* m_workload;
  uint32_t m_currentPage;
  Time m_pageStartTime;
  bool m_running;
};

/**
 * HTTP/1.0 serial client
 * Opens one connection per object, sequentially
 */
class Http10SerialClient : public HttpClientBase
{
public:
  static TypeId GetTypeId(void);
  Http10SerialClient();
  virtual ~Http10SerialClient();
  
protected:
  virtual void ProcessNextPage(void);
  
private:
  void SendRequest(uint32_t objectSize, bool isPrimary);
  void HandleRead(Ptr<Socket> socket);
  void HandleConnect(Ptr<Socket> socket);
  void HandleClose(Ptr<Socket> socket);
  
  Ptr<Socket> m_socket;
  uint32_t m_currentObject;
  uint32_t m_bytesReceived;
  uint32_t m_expectedBytes;
};

/**
 * HTTP/1.0 parallel client
 * Opens up to 8 concurrent connections
 */
class Http10ParallelClient : public HttpClientBase
{
public:
  static TypeId GetTypeId(void);
  Http10ParallelClient();
  virtual ~Http10ParallelClient();
  
protected:
  virtual void ProcessNextPage(void);
  
private:
  void SendRequest(uint32_t objectSize, bool isPrimary);
  void HandleRead(Ptr<Socket> socket);
  void HandleConnect(Ptr<Socket> socket);
  void HandleClose(Ptr<Socket> socket);
  
  uint32_t m_maxConnections;
  uint32_t m_objectsRemaining;
  std::vector<Ptr<Socket>> m_activeSockets;
  std::map<Ptr<Socket>, uint32_t> m_bytesReceived;
  std::map<Ptr<Socket>, uint32_t> m_expectedBytes;
};

/**
 * HTTP/1.1 persistent client
 * Uses up to 2 persistent connections
 */
class Http11PersistentClient : public HttpClientBase
{
public:
  static TypeId GetTypeId(void);
  Http11PersistentClient();
  virtual ~Http11PersistentClient();
  
protected:
  virtual void ProcessNextPage(void);
  
private:
  void SendRequest(uint32_t objectSize, bool isPrimary);
  void HandleRead(Ptr<Socket> socket);
  void HandleConnect(Ptr<Socket> socket);
  
  uint32_t m_maxConnections;
  uint32_t m_currentObject;
  uint32_t m_objectsRemaining;
  std::vector<Ptr<Socket>> m_persistentSockets;
  std::map<Ptr<Socket>, uint32_t> m_bytesReceived;
  std::map<Ptr<Socket>, uint32_t> m_expectedBytes;
};

/**
 * HTTP/1.1 pipelined client
 * Uses up to 2 persistent connections with pipelining
 */
class Http11PipelinedClient : public HttpClientBase
{
public:
  static TypeId GetTypeId(void);
  Http11PipelinedClient();
  virtual ~Http11PipelinedClient();
  
protected:
  virtual void ProcessNextPage(void);
  
private:
  void SendRequest(uint32_t objectSize, bool isPrimary);
  void HandleRead(Ptr<Socket> socket);
  void HandleConnect(Ptr<Socket> socket);
  
  uint32_t m_maxConnections;
  uint32_t m_maxPipeline;
  uint32_t m_objectsRemaining;
  std::vector<Ptr<Socket>> m_persistentSockets;
  std::map<Ptr<Socket>, uint32_t> m_bytesReceived;
  std::map<Ptr<Socket>, uint32_t> m_expectedBytes;
  std::map<Ptr<Socket>, uint32_t> m_pipelineDepth;
};

/**
 * SST HTTP client
 * Uses SST streams for parallel object retrieval
 */
class SstHttpClient : public HttpClientBase
{
public:
  static TypeId GetTypeId(void);
  SstHttpClient();
  virtual ~SstHttpClient();
  
protected:
  virtual void ProcessNextPage(void);
  
private:
  void SendRequest(uint32_t objectSize, bool isPrimary);
  void HandleRead(Ptr<SstStream> stream, Ptr<Packet> packet);
  
  Ptr<SstSocket> m_socket;
  uint32_t m_objectsRemaining;
  std::vector<Ptr<SstStream>> m_streams;
  std::map<Ptr<SstStream>, uint32_t> m_bytesReceived;
  std::map<Ptr<SstStream>, uint32_t> m_expectedBytes;
};

} // namespace ns3

#endif /* HTTP_CLIENTS_H */