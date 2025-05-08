/* http-clients.cc */

#include "http-clients.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/pointer.h"
#include "ns3/address.h"
#include "ns3/socket.h"
#include "ns3/packet.h"
#include <sstream>
#include <random>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("HttpClients");

/******************************
 * HttpClientBase Implementation
 ******************************/

NS_OBJECT_ENSURE_REGISTERED(HttpClientBase);

TypeId HttpClientBase::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::HttpClientBase")
    .SetParent<Application>()
    .AddAttribute("ServerAddress", "Server address",
                  AddressValue(),
                  MakeAddressAccessor(&HttpClientBase::m_serverAddress),
                  MakeAddressChecker());
  return tid;
}

HttpClientBase::HttpClientBase()
  : m_workload(nullptr), m_currentPage(0), m_running(false)
{
  NS_LOG_FUNCTION(this);
}

HttpClientBase::~HttpClientBase()
{
  NS_LOG_FUNCTION(this);
}

void HttpClientBase::SetWorkload(const WebWorkload* workload)
{
  m_workload = workload;
}

void HttpClientBase::SetServerAddress(Address serverAddress)
{
  m_serverAddress = serverAddress;
}

void HttpClientBase::DoDispose(void)
{
  NS_LOG_FUNCTION(this);
  Application::DoDispose();
}

void HttpClientBase::StartApplication(void)
{
  NS_LOG_FUNCTION(this);
  m_running = true;
  m_currentPage = 0;
  
  ProcessNextPage();
}

void HttpClientBase::StopApplication(void)
{
  NS_LOG_FUNCTION(this);
  m_running = false;
}

/******************************
 * Http10SerialClient Implementation
 ******************************/

NS_OBJECT_ENSURE_REGISTERED(Http10SerialClient);

TypeId Http10SerialClient::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::Http10SerialClient")
    .SetParent<HttpClientBase>()
    .AddConstructor<Http10SerialClient>();
  return tid;
}

Http10SerialClient::Http10SerialClient()
  : m_socket(nullptr), m_currentObject(0), m_bytesReceived(0), m_expectedBytes(0)
{
  NS_LOG_FUNCTION(this);
}

Http10SerialClient::~Http10SerialClient()
{
  NS_LOG_FUNCTION(this);
}

void Http10SerialClient::ProcessNextPage(void)
{
  NS_LOG_FUNCTION(this);
  
  if (!m_running || m_currentPage >= m_workload->GetPageCount()) {
    return;
  }
  
  const WebPage& page = m_workload->GetPage(m_currentPage);
  m_pageStartTime = Simulator::Now();
  m_currentObject = 0;
  
  // Request primary object first
  SendRequest(page.primaryObjectSize, true);
}

void Http10SerialClient::SendRequest(uint32_t objectSize, bool isPrimary)
{
  NS_LOG_FUNCTION(this << objectSize << isPrimary);
  
  // Create a new socket for each request (HTTP/1.0 style)
  m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
  m_socket->Bind();
  m_socket->Connect(m_serverAddress);
  
  m_socket->SetRecvCallback(MakeCallback(&Http10SerialClient::HandleRead, this));
  m_socket->SetConnectCallback(
    MakeCallback(&Http10SerialClient::HandleConnect, this),
    MakeCallback(&Http10SerialClient::HandleClose, this));
  
  // Send HTTP request
  std::ostringstream oss;
  oss << "GET /" << (isPrimary ? "main" : "embedded") << " HTTP/1.0\r\n"
      << "Host: example.com\r\n"
      << "Content-Length: " << objectSize << "\r\n"
      << "\r\n";
  std::string request = oss.str();
  
  Ptr<Packet> packet = Create<Packet>((uint8_t*)request.c_str(), request.size());
  m_socket->Send(packet);
  
  // Reset state for new request
  m_bytesReceived = 0;
  m_expectedBytes = objectSize;
}

void Http10SerialClient::HandleConnect(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  NS_LOG_INFO("Connected to server");
}

void Http10SerialClient::HandleClose(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  NS_LOG_INFO("Connection closed");
  
  // Move to next object or page
  const WebPage& page = m_workload->GetPage(m_currentPage);
  
  if (m_currentObject < page.embeddedObjectSizes.size()) {
    // Request next embedded object
    SendRequest(page.embeddedObjectSizes[m_currentObject], false);
    m_currentObject++;
  } else {
    // Page is complete, record metrics
    Time loadTime = Simulator::Now() - m_pageStartTime;
    
    NS_LOG_INFO("Page " << m_currentPage << " loaded in " << loadTime.GetMilliSeconds() 
                << " ms with " << page.GetObjectCount() << " objects and " 
                << page.GetTotalSize() << " bytes");
    
    // Record page load metrics for later analysis
    uint32_t requestCount = page.GetObjectCount();
    uint32_t totalSize = page.GetTotalSize();
    
    // Move to next page
    m_currentPage++;
    
    // Schedule next page with reading time delay
    // Use a simple exponential distribution for reading time
    std::exponential_distribution<double> readTimeDist(1.0/30.0); // Mean 30 seconds
    std::default_random_engine generator;
    double readTime = readTimeDist(generator);
    
    if (m_running) {
      Simulator::Schedule(Seconds(readTime), &Http10SerialClient::ProcessNextPage, this);
    }
  }
}

void Http10SerialClient::HandleRead(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  
  Ptr<Packet> packet;
  while ((packet = socket->Recv())) {
    m_bytesReceived += packet->GetSize();
    NS_LOG_INFO("Received " << packet->GetSize() << " bytes, total " << m_bytesReceived 
                << " of " << m_expectedBytes);
                
    // If we've received all expected bytes, we could close the connection
    // But let's let the server close it when it's done sending
  }
}

/******************************
 * Http10ParallelClient Implementation
 ******************************/

NS_OBJECT_ENSURE_REGISTERED(Http10ParallelClient);

TypeId Http10ParallelClient::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::Http10ParallelClient")
    .SetParent<HttpClientBase>()
    .AddConstructor<Http10ParallelClient>()
    .AddAttribute("MaxConnections", "Maximum number of parallel connections",
                  UintegerValue(8),
                  MakeUintegerAccessor(&Http10ParallelClient::m_maxConnections),
                  MakeUintegerChecker<uint32_t>());
  return tid;
}

Http10ParallelClient::Http10ParallelClient()
  : m_maxConnections(8), m_objectsRemaining(0)
{
  NS_LOG_FUNCTION(this);
}

Http10ParallelClient::~Http10ParallelClient()
{
  NS_LOG_FUNCTION(this);
}

void Http10ParallelClient::ProcessNextPage(void)
{
  NS_LOG_FUNCTION(this);
  
  if (!m_running || m_currentPage >= m_workload->GetPageCount()) {
    return;
  }
  
  const WebPage& page = m_workload->GetPage(m_currentPage);
  m_pageStartTime = Simulator::Now();
  m_objectsRemaining = 1 + page.embeddedObjectSizes.size();
  
  // Clear any previous state
  m_activeSockets.clear();
  m_bytesReceived.clear();
  m_expectedBytes.clear();
  
  // Request primary object first
  SendRequest(page.primaryObjectSize, true);
  
  // Request embedded objects up to max connections
  uint32_t startIndex = 0;
  uint32_t endIndex = std::min(m_maxConnections - 1, (uint32_t)page.embeddedObjectSizes.size());
  
  for (uint32_t i = startIndex; i < endIndex; i++) {
    SendRequest(page.embeddedObjectSizes[i], false);
  }
}

void Http10ParallelClient::SendRequest(uint32_t objectSize, bool isPrimary)
{
  NS_LOG_FUNCTION(this << objectSize << isPrimary);
  
  // Create a new socket for the request
  Ptr<Socket> socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
  socket->Bind();
  socket->Connect(m_serverAddress);
  
  socket->SetRecvCallback(MakeCallback(&Http10ParallelClient::HandleRead, this));
  socket->SetConnectCallback(
    MakeCallback(&Http10ParallelClient::HandleConnect, this),
    MakeCallback(&Http10ParallelClient::HandleClose, this));
  
  // Send HTTP request
  std::ostringstream oss;
  oss << "GET /" << (isPrimary ? "main" : "embedded") << " HTTP/1.0\r\n"
      << "Host: example.com\r\n"
      << "Content-Length: " << objectSize << "\r\n"
      << "\r\n";
  std::string request = oss.str();
  
  Ptr<Packet> packet = Create<Packet>((uint8_t*)request.c_str(), request.size());
  socket->Send(packet);
  
  // Initialize state for this socket
  m_activeSockets.push_back(socket);
  m_bytesReceived[socket] = 0;
  m_expectedBytes[socket] = objectSize;
}

void Http10ParallelClient::HandleConnect(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  NS_LOG_INFO("Connected to server");
}

void Http10ParallelClient::HandleClose(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  NS_LOG_INFO("Connection closed");
  
  // Remove socket from active list
  auto it = std::find(m_activeSockets.begin(), m_activeSockets.end(), socket);
  if (it != m_activeSockets.end()) {
    m_activeSockets.erase(it);
  }
  
  // Decrement objects remaining
  m_objectsRemaining--;
  
  // Start new connections if there are more objects to fetch
  const WebPage& page = m_workload->GetPage(m_currentPage);
  uint32_t fetchedCount = 1 + page.embeddedObjectSizes.size() - m_objectsRemaining;
  uint32_t remainingToStart = page.embeddedObjectSizes.size() - fetchedCount;
  
  if (remainingToStart > 0 && m_running) {
    uint32_t objectIndex = fetchedCount;
    SendRequest(page.embeddedObjectSizes[objectIndex - 1], false);
  }
  
  // Check if page is complete
  if (m_objectsRemaining == 0) {
    // Page is complete, record metrics
    Time loadTime = Simulator::Now() - m_pageStartTime;
    
    NS_LOG_INFO("Page " << m_currentPage << " loaded in " << loadTime.GetMilliSeconds() 
                << " ms with " << page.GetObjectCount() << " objects and " 
                << page.GetTotalSize() << " bytes");
    
    // Record page load metrics for later analysis
    uint32_t requestCount = page.GetObjectCount();
    uint32_t totalSize = page.GetTotalSize();
    
    // Move to next page
    m_currentPage++;
    
    // Schedule next page with reading time delay
    std::exponential_distribution<double> readTimeDist(1.0/30.0); // Mean 30 seconds
    std::default_random_engine generator;
    double readTime = readTimeDist(generator);
    
    if (m_running) {
      Simulator::Schedule(Seconds(readTime), &Http10ParallelClient::ProcessNextPage, this);
    }
  }
}

void Http10ParallelClient::HandleRead(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  
  Ptr<Packet> packet;
  while ((packet = socket->Recv())) {
    m_bytesReceived[socket] += packet->GetSize();
    NS_LOG_INFO("Received " << packet->GetSize() << " bytes, total " << m_bytesReceived[socket] 
                << " of " << m_expectedBytes[socket]);
                
    // If we've received all expected bytes, we could close the connection
    // But let's let the server close it when it's done sending
  }
}

/******************************
 * Http11PersistentClient Implementation
 ******************************/

NS_OBJECT_ENSURE_REGISTERED(Http11PersistentClient);

TypeId Http11PersistentClient::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::Http11PersistentClient")
    .SetParent<HttpClientBase>()
    .AddConstructor<Http11PersistentClient>()
    .AddAttribute("MaxConnections", "Maximum number of persistent connections",
                  UintegerValue(2),
                  MakeUintegerAccessor(&Http11PersistentClient::m_maxConnections),
                  MakeUintegerChecker<uint32_t>());
  return tid;
}

Http11PersistentClient::Http11PersistentClient()
  : m_maxConnections(2), m_currentObject(0), m_objectsRemaining(0)
{
  NS_LOG_FUNCTION(this);
}

Http11PersistentClient::~Http11PersistentClient()
{
  NS_LOG_FUNCTION(this);
}

void Http11PersistentClient::ProcessNextPage(void)
{
  NS_LOG_FUNCTION(this);
  
  if (!m_running || m_currentPage >= m_workload->GetPageCount()) {
    return;
  }
  
  const WebPage& page = m_workload->GetPage(m_currentPage);
  m_pageStartTime = Simulator::Now();
  m_currentObject = 0;
  m_objectsRemaining = 1 + page.embeddedObjectSizes.size();
  
  // Create persistent connections if needed
  while (m_persistentSockets.size() < m_maxConnections) {
    Ptr<Socket> socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
    socket->Bind();
    socket->Connect(m_serverAddress);
    
    socket->SetRecvCallback(MakeCallback(&Http11PersistentClient::HandleRead, this));
    socket->SetConnectCallback(
      MakeCallback(&Http11PersistentClient::HandleConnect, this),
      MakeCallback(&Http11PersistentClient::HandleRead, this)); // Use HandleRead as close callback
    
    m_persistentSockets.push_back(socket);
    m_bytesReceived[socket] = 0;
    m_expectedBytes[socket] = 0;
  }
  
  // Request primary object first
  SendRequest(page.primaryObjectSize, true);
}

void Http11PersistentClient::SendRequest(uint32_t objectSize, bool isPrimary)
{
  NS_LOG_FUNCTION(this << objectSize << isPrimary);
  
  // Find a socket with no active request
  Ptr<Socket> socket = nullptr;
  for (auto s : m_persistentSockets) {
    if (m_bytesReceived[s] >= m_expectedBytes[s]) {
      socket = s;
      break;
    }
  }
  
  if (!socket) {
    NS_LOG_WARN("No available socket, delaying request");
    return;
  }
  
  // Send HTTP request
  std::ostringstream oss;
  oss << "GET /" << (isPrimary ? "main" : "embedded") << " HTTP/1.1\r\n"
      << "Host: example.com\r\n"
      << "Content-Length: " << objectSize << "\r\n"
      << "\r\n";
  std::string request = oss.str();
  
  Ptr<Packet> packet = Create<Packet>((uint8_t*)request.c_str(), request.size());
  socket->Send(packet);
  
  // Reset state for new request
  m_bytesReceived[socket] = 0;
  m_expectedBytes[socket] = objectSize;
}

void Http11PersistentClient::HandleConnect(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  NS_LOG_INFO("Connected to server");
}

void Http11PersistentClient::HandleRead(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  
  Ptr<Packet> packet;
  while ((packet = socket->Recv())) {
    m_bytesReceived[socket] += packet->GetSize();
    NS_LOG_INFO("Received " << packet->GetSize() << " bytes, total " << m_bytesReceived[socket] 
                << " of " << m_expectedBytes[socket]);
                
    // Check if this request is complete
    if (m_bytesReceived[socket] >= m_expectedBytes[socket]) {
      // This request is complete
      m_objectsRemaining--;
      
      // Check if we need to send more requests
      const WebPage& page = m_workload->GetPage(m_currentPage);
      
      if (m_currentObject < page.embeddedObjectSizes.size()) {
        // Start next embedded object on this connection
        SendRequest(page.embeddedObjectSizes[m_currentObject], false);
        m_currentObject++;
      }
      
      // Check if page is complete
      if (m_objectsRemaining == 0) {
        // Page is complete, record metrics
        Time loadTime = Simulator::Now() - m_pageStartTime;
        
        NS_LOG_INFO("Page " << m_currentPage << " loaded in " << loadTime.GetMilliSeconds() 
                    << " ms with " << page.GetObjectCount() << " objects and " 
                    << page.GetTotalSize() << " bytes");
        
        // Record page load metrics for later analysis
        uint32_t requestCount = page.GetObjectCount();
        uint32_t totalSize = page.GetTotalSize();
        
        // Move to next page
        m_currentPage++;
        
        // Schedule next page with reading time delay
        std::exponential_distribution<double> readTimeDist(1.0/30.0); // Mean 30 seconds
        std::default_random_engine generator;
        double readTime = readTimeDist(generator);
        
        if (m_running) {
          Simulator::Schedule(Seconds(readTime), &Http11PersistentClient::ProcessNextPage, this);
        }
      }
    }
  }
}

/******************************
 * Http11PipelinedClient Implementation
 ******************************/

NS_OBJECT_ENSURE_REGISTERED(Http11PipelinedClient);

TypeId Http11PipelinedClient::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::Http11PipelinedClient")
    .SetParent<HttpClientBase>()
    .AddConstructor<Http11PipelinedClient>()
    .AddAttribute("MaxConnections", "Maximum number of persistent connections",
                  UintegerValue(2),
                  MakeUintegerAccessor(&Http11PipelinedClient::m_maxConnections),
                  MakeUintegerChecker<uint32_t>())
    .AddAttribute("MaxPipeline", "Maximum number of pipelined requests per connection",
                  UintegerValue(4),
                  MakeUintegerAccessor(&Http11PipelinedClient::m_maxPipeline),
                  MakeUintegerChecker<uint32_t>());
  return tid;
}

Http11PipelinedClient::Http11PipelinedClient()
  : m_maxConnections(2), m_maxPipeline(4), m_objectsRemaining(0)
{
  NS_LOG_FUNCTION(this);
}

Http11PipelinedClient::~Http11PipelinedClient()
{
  NS_LOG_FUNCTION(this);
}

void Http11PipelinedClient::ProcessNextPage(void)
{
  NS_LOG_FUNCTION(this);
  
  if (!m_running || m_currentPage >= m_workload->GetPageCount()) {
    return;
  }
  
  const WebPage& page = m_workload->GetPage(m_currentPage);
  m_pageStartTime = Simulator::Now();
  m_objectsRemaining = 1 + page.embeddedObjectSizes.size();
  
  // Create persistent connections if needed
  while (m_persistentSockets.size() < m_maxConnections) {
    Ptr<Socket> socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
    socket->Bind();
    socket->Connect(m_serverAddress);
    
    socket->SetRecvCallback(MakeCallback(&Http11PipelinedClient::HandleRead, this));
    socket->SetConnectCallback(
      MakeCallback(&Http11PipelinedClient::HandleConnect, this),
      MakeCallback(&Http11PipelinedClient::HandleRead, this)); // Use HandleRead as close callback
    
    m_persistentSockets.push_back(socket);
    m_bytesReceived[socket] = 0;
    m_expectedBytes[socket] = 0;
    m_pipelineDepth[socket] = 0;
  }
  
  // Request primary object first
  SendRequest(page.primaryObjectSize, true);
  
  // Request embedded objects up to pipeline depth
  uint32_t objectsStarted = 0;
  for (uint32_t i = 0; i < page.embeddedObjectSizes.size(); i++) {
    if (objectsStarted < m_maxConnections * m_maxPipeline) {
      SendRequest(page.embeddedObjectSizes[i], false);
      objectsStarted++;
    } else {
      break;
    }
  }
}

void Http11PipelinedClient::SendRequest(uint32_t objectSize, bool isPrimary)
{
  NS_LOG_FUNCTION(this << objectSize << isPrimary);
  
  // Find a socket with room in pipeline
  Ptr<Socket> socket = nullptr;
  for (auto s : m_persistentSockets) {
    if (m_pipelineDepth[s] < m_maxPipeline) {
      socket = s;
      break;
    }
  }
  
  if (!socket) {
    NS_LOG_WARN("No available socket, delaying request");
    return;
  }
  
  // Send HTTP request
  std::ostringstream oss;
  oss << "GET /" << (isPrimary ? "main" : "embedded") << " HTTP/1.1\r\n"
      << "Host: example.com\r\n"
      << "Content-Length: " << objectSize << "\r\n"
      << "\r\n";
  std::string request = oss.str();
  
  Ptr<Packet> packet = Create<Packet>((uint8_t*)request.c_str(), request.size());
  socket->Send(packet);
  
  // Increment pipeline depth
  m_pipelineDepth[socket]++;
  
  // Expected bytes is now a queue of responses
  m_expectedBytes[socket] += objectSize;
}

void Http11PipelinedClient::HandleConnect(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  NS_LOG_INFO("Connected to server");
}

void Http11PipelinedClient::HandleRead(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  
  Ptr<Packet> packet;
  while ((packet = socket->Recv())) {
    m_bytesReceived[socket] += packet->GetSize();
    NS_LOG_INFO("Received " << packet->GetSize() << " bytes, total " << m_bytesReceived[socket] 
                << " of " << m_expectedBytes[socket]);
                
    // Check if a complete request has been received
    const WebPage& page = m_workload->GetPage(m_currentPage);
    
    // If we've received a complete request
    if (m_bytesReceived[socket] >= m_expectedBytes[socket]) {
      // Decrement pipeline depth and objects remaining
      m_pipelineDepth[socket]--;
      m_objectsRemaining--;
      
      // Try to start next object if available
      uint32_t objectsStarted = 1 + page.embeddedObjectSizes.size() - m_objectsRemaining;
      if (objectsStarted < page.embeddedObjectSizes.size()) {
        SendRequest(page.embeddedObjectSizes[objectsStarted], false);
      }
      
      // Check if page is complete
      if (m_objectsRemaining == 0) {
        // Page is complete, record metrics
        Time loadTime = Simulator::Now() - m_pageStartTime;
        
        NS_LOG_INFO("Page " << m_currentPage << " loaded in " << loadTime.GetMilliSeconds() 
                    << " ms with " << page.GetObjectCount() << " objects and " 
                    << page.GetTotalSize() << " bytes");
        
        // Record page load metrics for later analysis
        uint32_t requestCount = page.GetObjectCount();
        uint32_t totalSize = page.GetTotalSize();
        
        // Move to next page
        m_currentPage++;
        
        // Schedule next page with reading time delay
        std::exponential_distribution<double> readTimeDist(1.0/30.0); // Mean 30 seconds
        std::default_random_engine generator;
        double readTime = readTimeDist(generator);
        
        if (m_running) {
          Simulator::Schedule(Seconds(readTime), &Http11PipelinedClient::ProcessNextPage, this);
        }
      }
    }
  }
}

/******************************
 * SstHttpClient Implementation
 ******************************/

NS_OBJECT_ENSURE_REGISTERED(SstHttpClient);

TypeId SstHttpClient::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::SstHttpClient")
    .SetParent<HttpClientBase>()
    .AddConstructor<SstHttpClient>();
  return tid;
}

SstHttpClient::SstHttpClient()
  : m_socket(0), m_objectsRemaining(0)
{
  NS_LOG_FUNCTION(this);
}

SstHttpClient::~SstHttpClient()
{
  NS_LOG_FUNCTION(this);
}

void SstHttpClient::ProcessNextPage(void)
{
  NS_LOG_FUNCTION(this);
  
  if (!m_running || m_currentPage >= m_workload->GetPageCount()) {
    return;
  }
  
  const WebPage& page = m_workload->GetPage(m_currentPage);
  m_pageStartTime = Simulator::Now();
  m_objectsRemaining = 1 + page.embeddedObjectSizes.size();
  m_streams.clear();
  m_bytesReceived.clear();
  m_expectedBytes.clear();
  
  // Create SST socket if needed
  if (!m_socket) {
    m_socket = CreateObject<SstSocket>();
    m_socket->Connect(m_serverAddress);
  }
  
  // Request primary object first
  SendRequest(page.primaryObjectSize, true);
  
  // Request all embedded objects in parallel
  for (uint32_t i = 0; i < page.embeddedObjectSizes.size(); i++) {
    SendRequest(page.embeddedObjectSizes[i], false);
  }
}

void SstHttpClient::SendRequest(uint32_t objectSize, bool isPrimary)
{
  NS_LOG_FUNCTION(this << objectSize << isPrimary);
  
  // Create a new stream for this request
  Ptr<SstStream> stream = m_socket->CreateSubstream();
  m_streams.push_back(stream);
  
  stream->SetRecvCallback(MakeCallback(&SstHttpClient::HandleRead, this));
  
  // Send HTTP request
  std::ostringstream oss;
  oss << "GET /" << (isPrimary ? "main" : "embedded") << " HTTP/1.0\r\n"
      << "Host: example.com\r\n"
      << "Content-Length: " << objectSize << "\r\n"
      << "\r\n";
  std::string request = oss.str();
  
  Ptr<Packet> packet = Create<Packet>((uint8_t*)request.c_str(), request.size());
  stream->Send(packet);
  
  // Initialize state for this stream
  m_bytesReceived[stream] = 0;
  m_expectedBytes[stream] = objectSize;
}

void SstHttpClient::HandleRead(Ptr<SstStream> stream, Ptr<Packet> packet)
{
  NS_LOG_FUNCTION(this << stream << packet);
  
  m_bytesReceived[stream] += packet->GetSize();
  NS_LOG_INFO("Received " << packet->GetSize() << " bytes, total " << m_bytesReceived[stream] 
              << " of " << m_expectedBytes[stream]);
              
  // Check if this stream has received all its data
  if (m_bytesReceived[stream] >= m_expectedBytes[stream]) {
    // Close the stream
    stream->Close();
    
    // Decrement objects remaining
    m_objectsRemaining--;
    
    // Check if page is complete
    if (m_objectsRemaining == 0) {
      // Page is complete, record metrics
      Time loadTime = Simulator::Now() - m_pageStartTime;
      
      NS_LOG_INFO("Page " << m_currentPage << " loaded in " << loadTime.GetMilliSeconds() 
                  << " ms with " << m_workload->GetPage(m_currentPage).GetObjectCount() 
                  << " objects and " << m_workload->GetPage(m_currentPage).GetTotalSize() 
                  << " bytes");
      
      // Record page load metrics for later analysis
      const WebPage& page = m_workload->GetPage(m_currentPage);
      uint32_t requestCount = page.GetObjectCount();
      uint32_t totalSize = page.GetTotalSize();
      
      // Move to next page
      m_currentPage++;
      
      // Schedule next page with reading time delay
      std::exponential_distribution<double> readTimeDist(1.0/30.0); // Mean 30 seconds
      std::default_random_engine generator;
      double readTime = readTimeDist(generator);
      
      if (m_running) {
        Simulator::Schedule(Seconds(readTime), &SstHttpClient::ProcessNextPage, this);
      }
    }
  }
}

} // namespace ns3