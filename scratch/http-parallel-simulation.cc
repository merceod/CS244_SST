/* http-parallel-simulation.cc
 * 
 * HTTP/1.0 parallel mode simulation using UCB web trace data
 * Opens up to 8 concurrent TCP connections, one request per connection
 */

 #include "ns3/applications-module.h"
 #include "ns3/core-module.h"
 #include "ns3/internet-module.h"
 #include "ns3/network-module.h"
 #include "ns3/point-to-point-module.h"
 #include "ns3/flow-monitor-module.h"
 #include <fstream>
 #include <string>
 #include <vector>
 #include <iostream>
 #include <sstream>
 #include <map>
 #include <queue>
 #include <algorithm>
 
 using namespace ns3;
 
 NS_LOG_COMPONENT_DEFINE("HttpParallelSimulation");
 
 // Define a structure to hold web request data
 struct WebRequest {
   uint32_t id;           // Request ID
   std::string url;       // Request URL
   uint32_t size;         // Response size in bytes
   bool isPrimary;        // Is this a primary (HTML) request
   Time startTime;        // When the request was started
   Time completeTime;     // When the request was completed
 };
 
 // Define a structure to hold a web page with its requests
 struct WebPage {
   std::vector<WebRequest> requests;
   bool isComplete;
   uint32_t primaryRequestId;
   bool primaryCompleted;
   
   WebPage() : isComplete(false), primaryRequestId(0), primaryCompleted(false) {}
 };
 
 // Structure to track a parallel connection
 struct ParallelConnection {
   Ptr<Socket> socket;
   bool isActive;
   bool isConnecting;
   WebRequest* currentRequest;
   uint32_t totalBytes;
   uint32_t pendingBytes;
   std::string receiveBuffer;
   bool inHeader;
   uint32_t expectedBytes;
   
   ParallelConnection() : socket(nullptr), isActive(false), isConnecting(false), 
                         currentRequest(nullptr), totalBytes(0), pendingBytes(0),
                         inHeader(true), expectedBytes(0) {}
 };
 
 // HTTP/1.0 parallel client application
 class HttpParallelClient : public Application {
 public:
   HttpParallelClient() : m_running(false), m_currentPageIndex(0), 
                         m_maxConnections(8), m_waitingForPrimary(false) {}
   virtual ~HttpParallelClient() {}
 
   static TypeId GetTypeId() {
     static TypeId tid = TypeId("ns3::HttpParallelClient")
       .SetParent<Application>()
       .SetGroupName("Applications")
       .AddConstructor<HttpParallelClient>();
     return tid;
   }
 
   void SetPages(std::vector<WebPage> pages) {
     m_pages = pages;
   }
 
   void SetServer(Address address) {
     m_serverAddress = address;
   }
 
   std::vector<WebPage> GetCompletedPages() const {
     return m_pages;
   }
 
 protected:
   virtual void DoDispose() {
     for (uint32_t i = 0; i < m_connections.size(); i++) {
       CleanupConnection(i);
     }
     Application::DoDispose();
   }
 
   virtual void StartApplication() {
     NS_LOG_FUNCTION(this);
     m_running = true;
     
     // Initialize connection pool
     m_connections.resize(m_maxConnections);
     
     ProcessNextPage();
   }
 
   virtual void StopApplication() {
     NS_LOG_FUNCTION(this);
     m_running = false;
     
     for (uint32_t i = 0; i < m_connections.size(); i++) {
       CleanupConnection(i);
     }
   }
 
 private:
   void CleanupConnection(uint32_t connIndex) {
     if (connIndex >= m_connections.size()) return;
     
     ParallelConnection& conn = m_connections[connIndex];
     
     if (conn.socket) {
       // Clear all callbacks before closing
       conn.socket->SetConnectCallback(
         MakeNullCallback<void, Ptr<Socket>>(),
         MakeNullCallback<void, Ptr<Socket>>()
       );
       conn.socket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
       conn.socket->SetCloseCallbacks(
         MakeNullCallback<void, Ptr<Socket>>(),
         MakeNullCallback<void, Ptr<Socket>>()
       );
       
       conn.socket->Close();
       conn.socket = nullptr;
     }
     
     conn.isActive = false;
     conn.isConnecting = false;
     conn.currentRequest = nullptr;
     conn.totalBytes = 0;
     conn.pendingBytes = 0;
     conn.receiveBuffer.clear();
     conn.inHeader = true;
     conn.expectedBytes = 0;
   }
 
   void ProcessNextPage() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       NS_LOG_INFO("All pages processed");
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     
     if (page.requests.empty()) {
       NS_LOG_WARN("Empty page found at index " << m_currentPageIndex);
       page.isComplete = true;
       m_currentPageIndex++;
       Simulator::Schedule(MicroSeconds(1), &HttpParallelClient::ProcessNextPage, this);
       return;
     }
     
     m_pageStartTime = Simulator::Now();
     
     // Ensure primary request is first
     for (size_t i = 0; i < page.requests.size(); i++) {
       if (page.requests[i].isPrimary) {
         if (i != 0) {
           std::swap(page.requests[0], page.requests[i]);
         }
         page.primaryRequestId = page.requests[0].id;
         break;
       }
     }
     
     // Reset page state
     page.primaryCompleted = false;
     page.isComplete = false;
     m_waitingForPrimary = true;
     
     // Clear pending requests queue
     while (!m_pendingRequests.empty()) {
       m_pendingRequests.pop();
     }
     
     NS_LOG_INFO("Starting page " << m_currentPageIndex << " with " << page.requests.size() << " requests");
     
     // Start with primary request
     StartPrimaryRequest();
     
     // Set a timeout for the entire page
     Simulator::Schedule(Seconds(30), &HttpParallelClient::HandlePageTimeout, this, m_currentPageIndex);
   }
 
   void StartPrimaryRequest() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     if (page.requests.empty()) return;
     
     NS_LOG_INFO("Starting primary request for page " << m_currentPageIndex);
     
     // Find an available connection and start the primary request
     StartRequest(&page.requests[0]);
   }
 
   void StartSecondaryRequests() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     if (page.requests.size() <= 1) return;
     
     NS_LOG_INFO("Starting " << (page.requests.size() - 1) << " secondary requests for page " << m_currentPageIndex);
     
     // Queue all secondary requests
     for (size_t i = 1; i < page.requests.size(); i++) {
       m_pendingRequests.push(&page.requests[i]);
     }
     
     // Process the queue
     ProcessPendingRequests();
   }
 
   void ProcessPendingRequests() {
     if (!m_running) return;
     
     // Try to start as many pending requests as we have available connections
     while (!m_pendingRequests.empty() && HasAvailableConnection()) {
       WebRequest* req = m_pendingRequests.front();
       m_pendingRequests.pop();
       StartRequest(req);
     }
   }
 
   bool HasAvailableConnection() {
     for (const auto& conn : m_connections) {
       if (!conn.isActive && !conn.isConnecting) {
         return true;
       }
     }
     return false;
   }
 
   int GetAvailableConnectionIndex() {
     for (size_t i = 0; i < m_connections.size(); i++) {
       if (!m_connections[i].isActive && !m_connections[i].isConnecting) {
         return i;
       }
     }
     return -1;
   }
 
   void StartRequest(WebRequest* request) {
     int connIndex = GetAvailableConnectionIndex();
     if (connIndex < 0) {
       NS_LOG_WARN("No available connections, queueing request");
       m_pendingRequests.push(request);
       return;
     }
     
     ParallelConnection& conn = m_connections[connIndex];
     
     // Extra safety check
     if (conn.isActive || conn.isConnecting || conn.currentRequest || conn.socket) {
       NS_LOG_ERROR("Connection " << connIndex << " is not properly cleaned up");
       CleanupConnection(connIndex);
     }
     
     conn.currentRequest = request;
     conn.isConnecting = true;
     
     // Create new socket for this request
     conn.socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
     conn.socket->Bind();
     
     // Set up callbacks
     conn.socket->SetConnectCallback(
       MakeCallback(&HttpParallelClient::ConnectionSucceeded, this, connIndex),
       MakeCallback(&HttpParallelClient::ConnectionFailed, this, connIndex)
     );
     conn.socket->SetRecvCallback(
       MakeCallback(&HttpParallelClient::HandleRead, this, connIndex)
     );
     conn.socket->SetCloseCallbacks(
       MakeCallback(&HttpParallelClient::HandleClose, this, connIndex),
       MakeCallback(&HttpParallelClient::HandleClose, this, connIndex)
     );
     
     // Connect to server
     conn.socket->Connect(m_serverAddress);
     
     NS_LOG_INFO("Starting connection " << connIndex << " for request " 
                 << (request->isPrimary ? "[PRIMARY]" : "[SECONDARY]") 
                 << " URL: " << request->url);
   }
 
   void ConnectionSucceeded(uint32_t connIndex, Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << connIndex);
     
     if (!m_running || connIndex >= m_connections.size()) return;
     
     ParallelConnection& conn = m_connections[connIndex];
     
     // Check if this connection is still valid
     if (!conn.isConnecting || conn.socket != socket) {
       NS_LOG_WARN("Stale connection callback for connection " << connIndex);
       return;
     }
     
     conn.isActive = true;
     conn.isConnecting = false;
     
     if (!conn.currentRequest) {
       NS_LOG_ERROR("Connection succeeded but no current request");
       CleanupConnection(connIndex);
       return;
     }
     
     conn.currentRequest->startTime = Simulator::Now();
     
     // Extract path from URL
     std::string path = conn.currentRequest->url;
     std::istringstream iss(conn.currentRequest->url);
     std::string method, extractedPath, version;
     if (iss >> method >> extractedPath >> version) {
       path = extractedPath;
     }
     
     // Send HTTP/1.0 request
     std::ostringstream oss;
     oss << "GET " << path << "?size=" << conn.currentRequest->size << " HTTP/1.0\r\n"
         << "Host: example.com\r\n"
         << "User-Agent: ns3-http-parallel-client\r\n"
         << "Connection: close\r\n"
         << "\r\n";
     std::string request = oss.str();
     
     Ptr<Packet> packet = Create<Packet>((uint8_t*) request.c_str(), request.size());
     int result = socket->Send(packet);
     
     if (result == -1) {
       NS_LOG_ERROR("Failed to send request");
     } else {
       NS_LOG_INFO("Connection " << connIndex << " sent request for " 
                   << conn.currentRequest->url << " (size=" << conn.currentRequest->size << ")"
                   << (conn.currentRequest->isPrimary ? " [PRIMARY]" : " [SECONDARY]"));
     }
     
     // Set up expected response
     conn.pendingBytes = conn.currentRequest->size;
     conn.totalBytes = 0;
     conn.receiveBuffer.clear();
     conn.inHeader = true;
     conn.expectedBytes = 0;
   }
 
   void ConnectionFailed(uint32_t connIndex, Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << connIndex);
     
     if (!m_running || connIndex >= m_connections.size()) return;
     
     ParallelConnection& conn = m_connections[connIndex];
     
     // Check if this is a stale callback
     if (!conn.isConnecting || conn.socket != socket) {
       NS_LOG_WARN("Stale connection failed callback for connection " << connIndex);
       return;
     }
     
     NS_LOG_ERROR("Connection " << connIndex << " failed");
     
     // Put request back in queue
     if (conn.currentRequest) {
       m_pendingRequests.push(conn.currentRequest);
     }
     
     CleanupConnection(connIndex);
     
     // Try to process pending requests with other connections
     Simulator::Schedule(MicroSeconds(10), &HttpParallelClient::ProcessPendingRequests, this);
   }
 
   void HandleRead(uint32_t connIndex, Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << connIndex);
     
     if (!m_running || connIndex >= m_connections.size()) return;
     
     ParallelConnection& conn = m_connections[connIndex];
     
     // Check if this is a stale callback
     if (!conn.isActive || conn.socket != socket) {
       NS_LOG_WARN("Stale read callback for connection " << connIndex);
       return;
     }
     
     Ptr<Packet> packet;
     Address from;
     
     while ((packet = socket->RecvFrom(from))) {
       uint32_t packetSize = packet->GetSize();
       uint8_t* buffer = new uint8_t[packetSize];
       packet->CopyData(buffer, packetSize);
       
       conn.receiveBuffer.append((char*)buffer, packetSize);
       delete[] buffer;
       
       ProcessResponse(connIndex);
     }
   }
 
   void ProcessResponse(uint32_t connIndex) {
     if (connIndex >= m_connections.size()) return;
     
     ParallelConnection& conn = m_connections[connIndex];
     
     if (conn.inHeader) {
       // Look for end of headers
       size_t headerEnd = conn.receiveBuffer.find("\r\n\r\n");
       if (headerEnd == std::string::npos) {
         return; // Wait for more data
       }
       
       // Parse Content-Length
       std::string headers = conn.receiveBuffer.substr(0, headerEnd);
       size_t clPos = headers.find("Content-Length:");
       if (clPos != std::string::npos) {
         clPos += 15;
         while (clPos < headers.length() && (headers[clPos] == ' ' || headers[clPos] == '\t')) clPos++;
         
         std::string lengthStr;
         while (clPos < headers.length() && isdigit(headers[clPos])) {
           lengthStr += headers[clPos];
           clPos++;
         }
         
         if (!lengthStr.empty()) {
           try {
             conn.expectedBytes = std::stoul(lengthStr);
           } catch (const std::exception& e) {
             NS_LOG_WARN("Invalid Content-Length value: " << lengthStr);
             conn.expectedBytes = 0;
           }
         }
       }
       
       // Remove headers from buffer
       conn.receiveBuffer.erase(0, headerEnd + 4);
       conn.inHeader = false;
       conn.totalBytes = 0;
     }
     
     // Process body
     uint32_t bodyBytes = conn.receiveBuffer.length();
     conn.totalBytes += bodyBytes;
     conn.receiveBuffer.clear(); // We don't need to store the body
     
     // Check if connection is still valid (defensive programming)
     if (!conn.currentRequest) {
       NS_LOG_WARN("Connection " << connIndex << " has no current request during response processing");
       return;
     }
     
     NS_LOG_DEBUG("Connection " << connIndex << " received " << bodyBytes 
                  << " bytes (total: " << conn.totalBytes 
                  << "/" << conn.expectedBytes << ")");
     
     // Check if response is complete
     if (conn.totalBytes >= conn.expectedBytes && conn.currentRequest) {
       conn.currentRequest->completeTime = Simulator::Now();
       Time responseTime = conn.currentRequest->completeTime - conn.currentRequest->startTime;
       
       bool isPrimary = conn.currentRequest->isPrimary;
       
       NS_LOG_INFO("Connection " << connIndex << " completed request in " 
                   << responseTime.GetSeconds() << " seconds"
                   << (isPrimary ? " [PRIMARY]" : " [SECONDARY]"));
       
       if (isPrimary) {
         HandlePrimaryRequestComplete();
       }
       
       // Clean up this connection
       CleanupConnection(connIndex);
       
       // Process more pending requests
       ProcessPendingRequests();
       
       // Check if page is complete
       CheckPageComplete();
     }
   }
 
   void HandlePrimaryRequestComplete() {
     if (m_currentPageIndex >= m_pages.size()) return;
     
     WebPage& page = m_pages[m_currentPageIndex];
     page.primaryCompleted = true;
     m_waitingForPrimary = false;
     
     NS_LOG_INFO("Primary request completed for page " << m_currentPageIndex << " - starting secondary requests");
     
     StartSecondaryRequests();
   }
 
   void CheckPageComplete() {
     if (m_currentPageIndex >= m_pages.size()) return;
     
     WebPage& page = m_pages[m_currentPageIndex];
     
     // Count completed requests
     uint32_t completedRequests = 0;
     for (const auto& req : page.requests) {
       if (!req.completeTime.IsZero()) {
         completedRequests++;
       }
     }
     
     // Check if all requests are complete
     if (completedRequests >= page.requests.size()) {
       page.isComplete = true;
       
       // Calculate page load time
       Time pageStartTime = Seconds(0);
       Time pageEndTime = Seconds(0);
       
       // Find primary request start time
       for (const auto& req : page.requests) {
         if (req.isPrimary && !req.startTime.IsZero()) {
           pageStartTime = req.startTime;
           break;
         }
       }
       
       // Find latest completion time
       for (const auto& req : page.requests) {
         if (!req.completeTime.IsZero() && 
             (pageEndTime.IsZero() || req.completeTime > pageEndTime)) {
           pageEndTime = req.completeTime;
         }
       }
       
       if (!pageStartTime.IsZero() && !pageEndTime.IsZero()) {
         double pageTime = (pageEndTime - pageStartTime).GetSeconds();
         NS_LOG_INFO("Page " << m_currentPageIndex << " completed in " 
                     << pageTime << " seconds (all " << completedRequests << " requests done)");
       }
       
       m_currentPageIndex++;
       Simulator::Schedule(MicroSeconds(10), &HttpParallelClient::ProcessNextPage, this);
     }
   }
 
   void HandlePageTimeout(uint32_t pageIndex) {
     if (!m_running || pageIndex != m_currentPageIndex) {
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     
     uint32_t completedRequests = 0;
     for (const auto& req : page.requests) {
       if (!req.completeTime.IsZero()) {
         completedRequests++;
       }
     }
     
     NS_LOG_WARN("Page " << m_currentPageIndex << " timeout - only " << completedRequests 
                 << "/" << page.requests.size() << " requests completed. Moving to next page.");
     
     // Mark incomplete requests as completed (for statistics)
     for (auto& req : page.requests) {
       if (req.completeTime.IsZero()) {
         req.completeTime = Simulator::Now();
       }
     }
     
     // Cancel all active connections
     for (uint32_t i = 0; i < m_connections.size(); i++) {
       if (m_connections[i].isActive || m_connections[i].isConnecting) {
         CleanupConnection(i);
       }
     }
     
     page.isComplete = true;
     m_currentPageIndex++;
     Simulator::Schedule(MicroSeconds(10), &HttpParallelClient::ProcessNextPage, this);
   }
 
   void HandleClose(uint32_t connIndex, Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << connIndex);
     
     if (connIndex >= m_connections.size()) return;
     
     ParallelConnection& conn = m_connections[connIndex];
     
     // Check if this is a stale callback
     if (conn.socket != socket) {
       NS_LOG_WARN("Stale close callback for connection " << connIndex);
       return;
     }
     
     // Connection closed - might be normal after response is complete
     if (conn.isActive && conn.currentRequest && conn.currentRequest->completeTime.IsZero()) {
       // Unexpected close
       NS_LOG_WARN("Connection " << connIndex << " closed unexpectedly");
       
       // Put request back in queue to retry
       m_pendingRequests.push(conn.currentRequest);
     }
     
     CleanupConnection(connIndex);
     
     // Try to process more pending requests
     ProcessPendingRequests();
   }
 
   bool m_running;
   Address m_serverAddress;
   std::vector<WebPage> m_pages;
   uint32_t m_currentPageIndex;
   std::vector<ParallelConnection> m_connections;
   uint32_t m_maxConnections;
   std::queue<WebRequest*> m_pendingRequests;
   Time m_pageStartTime;
   bool m_waitingForPrimary;
 };
 
 // HTTP server application (same as in serial version)
 class HttpServer : public Application {
 public:
   HttpServer() : m_socket(nullptr), m_running(false) {}
   virtual ~HttpServer() {}
 
   static TypeId GetTypeId() {
     static TypeId tid = TypeId("ns3::HttpServer")
       .SetParent<Application>()
       .SetGroupName("Applications")
       .AddConstructor<HttpServer>();
     return tid;
   }
 
   void SetPort(uint16_t port) {
     m_port = port;
   }
 
 protected:
   virtual void DoDispose() {
     if (m_socket) {
       m_socket->Close();
       m_socket = nullptr;
     }
     
     for (auto it = m_socketList.begin(); it != m_socketList.end(); ++it) {
       (*it)->Close();
     }
     m_socketList.clear();
     
     Application::DoDispose();
   }
 
   virtual void StartApplication() {
     NS_LOG_FUNCTION(this);
     m_running = true;
     
     if (!m_socket) {
       m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
       InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
       m_socket->Bind(local);
       m_socket->Listen();
       
       m_socket->SetAcceptCallback(
         MakeNullCallback<bool, Ptr<Socket>, const Address &>(),
         MakeCallback(&HttpServer::HandleAccept, this)
       );
     }
     
     NS_LOG_INFO("HTTP server listening on port " << m_port);
   }
 
   virtual void StopApplication() {
     NS_LOG_FUNCTION(this);
     m_running = false;
     
     if (m_socket) {
       m_socket->Close();
       m_socket = nullptr;
     }
     
     for (auto it = m_socketList.begin(); it != m_socketList.end(); ++it) {
       (*it)->Close();
     }
     m_socketList.clear();
   }
 
 private:
   void HandleAccept(Ptr<Socket> socket, const Address& from) {
     NS_LOG_FUNCTION(this << socket << from);
     
     socket->SetRecvCallback(MakeCallback(&HttpServer::HandleRead, this));
     m_socketList.push_back(socket);
     
     NS_LOG_INFO("Server accepted connection from " 
                 << InetSocketAddress::ConvertFrom(from).GetIpv4());
   }
 
   void HandleRead(Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << socket);
     
     Ptr<Packet> packet;
     Address from;
     
     while ((packet = socket->RecvFrom(from))) {
       uint8_t buffer[2048];
       uint32_t size = std::min(packet->GetSize(), (uint32_t)2048);
       packet->CopyData(buffer, size);
       buffer[size] = '\0';
       
       std::string request((char*)buffer, size);
       
       NS_LOG_INFO("Server received request: " << size << " bytes");
       
       // Parse the request
       std::istringstream iss(request);
       std::string method, path, version;
       if (iss >> method >> path >> version) {
         SendResponse(socket, path);
       }
     }
   }
 
   void SendResponse(Ptr<Socket> socket, const std::string& url) {
     // Determine response size first
     uint32_t responseSize = 1024;
     
     size_t pos = url.find("size=");
     if (pos != std::string::npos) {
       try {
         std::string sizeStr = url.substr(pos + 5);
         size_t endPos = sizeStr.find_first_of(" \t\r\n&");
         if (endPos != std::string::npos) {
           sizeStr = sizeStr.substr(0, endPos);
         }
         responseSize = std::stoul(sizeStr);
       } catch (const std::exception& e) {
         NS_LOG_WARN("Invalid size in URL: " << url);
       }
     }
     
     // Send HTTP headers with Content-Length
     std::ostringstream header;
     header << "HTTP/1.0 200 OK\r\n"
            << "Content-Type: text/html\r\n"
            << "Content-Length: " << responseSize << "\r\n"
            << "Connection: close\r\n"
            << "\r\n";
     std::string headerStr = header.str();
     
     Ptr<Packet> headerPacket = Create<Packet>((uint8_t*) headerStr.c_str(), headerStr.size());
     socket->Send(headerPacket);
     
     NS_LOG_INFO("Server sending response of " << responseSize << " bytes");
     
     // Send response body
     uint32_t chunkSize = 1400;
     uint32_t remaining = responseSize;
     
     while (remaining > 0 && socket->GetTxAvailable() > 0) {
       uint32_t currentChunk = std::min(remaining, chunkSize);
       
       uint8_t* buffer = new uint8_t[currentChunk];
       memset(buffer, 'X', currentChunk);
       
       Ptr<Packet> dataPacket = Create<Packet>(buffer, currentChunk);
       socket->Send(dataPacket);
       
       delete[] buffer;
       remaining -= currentChunk;
       
       if (remaining > 0) {
         Simulator::Schedule(MicroSeconds(1), &HttpServer::SendRemainingData, 
                            this, socket, remaining, chunkSize);
         break;
       }
     }
   }
 
   void SendRemainingData(Ptr<Socket> socket, uint32_t remaining, uint32_t chunkSize) {
     bool socketValid = false;
     for (auto it = m_socketList.begin(); it != m_socketList.end(); ++it) {
       if (*it == socket) {
         socketValid = true;
         break;
       }
     }
     
     if (!socketValid || !m_running) {
       return;
     }
     
     uint32_t currentChunk = std::min(remaining, chunkSize);
     
     if (socket->GetTxAvailable() > 0) {
       uint8_t* buffer = new uint8_t[currentChunk];
       memset(buffer, 'X', currentChunk);
       
       Ptr<Packet> dataPacket = Create<Packet>(buffer, currentChunk);
       socket->Send(dataPacket);
       
       delete[] buffer;
       remaining -= currentChunk;
       
       if (remaining > 0) {
         Simulator::Schedule(MicroSeconds(1), &HttpServer::SendRemainingData, 
                           this, socket, remaining, chunkSize);
       }
     }
   }
 
   Ptr<Socket> m_socket;
   std::list<Ptr<Socket>> m_socketList;
   uint16_t m_port;
   bool m_running;
 };
 
 // Read trace file function (same as serial version)
 std::vector<WebPage> ReadTraceFile(const std::string& filename) {
   std::vector<WebPage> pages;
   WebPage currentPage;
 
   uint32_t id = 0;
   
   std::ifstream file(filename);
   if (file.is_open()) {
     std::string line;
     
     while (std::getline(file, line)) {
       if (line.empty() || line[0] == '#') {
         if (line.find("End of Page") != std::string::npos) {
           if (!currentPage.requests.empty()) {
             pages.push_back(currentPage);
             currentPage = WebPage();
           }
         }
         continue;
       }
       
       std::istringstream iss(line);
       WebRequest req;
       req.id = id++;
       
       std::string url, size, isPrimary, requestTime, responseTime;
       if (std::getline(iss, url, ',') && 
           std::getline(iss, size, ',') && 
           std::getline(iss, isPrimary, ',') &&
           std::getline(iss, requestTime, ',') &&
           std::getline(iss, responseTime)) {
         
         req.url = url;
         
         try {
           req.size = std::stoi(size);
         } catch (const std::exception& e) {
           NS_LOG_WARN("Invalid size value in trace file: " << size);
           req.size = 1024;
         }
         
         req.isPrimary = (isPrimary == "1" || isPrimary == "true");
         
         currentPage.requests.push_back(req);
       }
     }
     
     if (!currentPage.requests.empty()) {
       pages.push_back(currentPage);
     }
     
     file.close();
   } else {
     NS_LOG_WARN("Could not open trace file: " << filename);
   }
   
   return pages;
 }
 
 // Main function
 int main(int argc, char* argv[]) {
   Time::SetResolution(Time::US);
   std::string traceFile = "";
   std::string bandwidth = "1.5Mbps";
   std::string delay = "25ms";
   double simulationTime = 500.0;
   uint32_t maxPages = 0;
   
   CommandLine cmd(__FILE__);
   cmd.AddValue("traceFile", "Path to trace file", traceFile);
   cmd.AddValue("bandwidth", "Bandwidth of the link", bandwidth);
   cmd.AddValue("delay", "Delay of the link", delay);
   cmd.AddValue("time", "Simulation time in seconds", simulationTime);
   cmd.AddValue("maxPages", "Maximum number of pages to process (0 for all)", maxPages);
   cmd.Parse(argc, argv);
   
   if (traceFile.empty()) {
     std::cout << "Error: No trace file specified. Use --traceFile=<filename>" << std::endl;
     return 1;
   }
   
   std::cout << "Using trace file: " << traceFile << std::endl;
   
   LogComponentEnable("HttpParallelSimulation", LOG_LEVEL_INFO);
   
   // Create nodes
   NodeContainer nodes;
   nodes.Create(2);
   
   // Create point-to-point link
   PointToPointHelper pointToPoint;
   pointToPoint.SetDeviceAttribute("DataRate", StringValue(bandwidth));
   pointToPoint.SetChannelAttribute("Delay", StringValue(delay));
   
   NetDeviceContainer devices = pointToPoint.Install(nodes);
   
   // Install Internet stack
   InternetStackHelper internet;
   internet.Install(nodes);
   
   // Assign IP addresses
   Ipv4AddressHelper address;
   address.SetBase("10.1.1.0", "255.255.255.0");
   Ipv4InterfaceContainer interfaces = address.Assign(devices);
   
   // Read trace data
   std::vector<WebPage> allPages = ReadTraceFile(traceFile);
   
   if (allPages.empty()) {
     std::cout << "Error: No pages loaded from trace file: " << traceFile << std::endl;
     return 1;
   }
   
   std::cout << "Successfully loaded " << allPages.size() << " pages from trace file" << std::endl;
   
   // Limit pages if specified
   std::vector<WebPage> pages;
   if (maxPages > 0 && allPages.size() > maxPages) {
     std::cout << "Limiting simulation to " << maxPages << " pages out of " 
               << allPages.size() << " total pages" << std::endl;
     pages.assign(allPages.begin(), allPages.begin() + maxPages);
   } else {
     pages = allPages;
   }
   
   NS_LOG_INFO("Loaded " << pages.size() << " web pages from trace");
   
   // Create and install HTTP server
   uint16_t port = 80;
   Ptr<HttpServer> server = CreateObject<HttpServer>();
   server->SetPort(port);
   nodes.Get(1)->AddApplication(server);
   server->SetStartTime(Seconds(1.0));
   server->SetStopTime(Seconds(simulationTime));
   
   // Create and install HTTP client
   Ptr<HttpParallelClient> client = CreateObject<HttpParallelClient>();
   Address serverAddress(InetSocketAddress(interfaces.GetAddress(1), port));
   client->SetServer(serverAddress);
   client->SetPages(pages);
   nodes.Get(0)->AddApplication(client);
   client->SetStartTime(Seconds(2.0));
   client->SetStopTime(Seconds(simulationTime));
   
   // Enable packet tracing
   AsciiTraceHelper ascii;
   pointToPoint.EnableAsciiAll(ascii.CreateFileStream("http-parallel-simulation.tr"));
   pointToPoint.EnablePcapAll("http-parallel-simulation");
   
   // Set up flow monitor
   Ptr<FlowMonitor> flowMonitor;
   FlowMonitorHelper flowHelper;
   flowMonitor = flowHelper.InstallAll();
   
   // Run simulation
   NS_LOG_INFO("Running HTTP/1.0 parallel simulation for " << simulationTime << " seconds");
   Simulator::Stop(Seconds(simulationTime));
   Simulator::Run();
   
   // Process statistics
   std::cout << "Results for HTTP/1.0 parallel mode:" << std::endl;
   std::cout << "------------------------------------" << std::endl;
   
   std::vector<WebPage> completedPages = client->GetCompletedPages();
   
   uint32_t completedPageCount = 0;
   double totalPageTime = 0.0;
   uint32_t totalCompletedRequests = 0;
   double totalRequestTime = 0.0;
   
   for (const auto& page : completedPages) {
     bool pageHasEndTime = false;
     Time pageStartTime = Seconds(0);
     Time pageEndTime = Seconds(0);
     uint32_t pageCompletedRequests = 0;
     uint32_t totalPageSize = 0;
     uint32_t completedPageSize = 0;
     
     Time earliestStartTime = Seconds(0);
     bool foundStartTime = false;
     
     for (const auto& req : page.requests) {
       totalPageSize += req.size;
       
       if (!req.startTime.IsZero()) {
         if (!foundStartTime || req.startTime < earliestStartTime) {
           earliestStartTime = req.startTime;
           foundStartTime = true;
         }
       }
       
       if (!req.completeTime.IsZero() && req.completeTime > Seconds(0)) {
         pageCompletedRequests++;
         completedPageSize += req.size;
         
         if (!req.startTime.IsZero()) {
           Time requestTime = req.completeTime - req.startTime;
           if (requestTime.GetSeconds() > 0) {
             totalRequestTime += requestTime.GetSeconds();
           }
         }
         
         if (pageEndTime.IsZero() || req.completeTime > pageEndTime) {
           pageEndTime = req.completeTime;
           pageHasEndTime = true;
         }
       }
     }
     
     if (foundStartTime) {
       pageStartTime = earliestStartTime;
     }
     
     if (foundStartTime && pageHasEndTime && pageEndTime > pageStartTime && pageCompletedRequests > 0) {
       double pageTime = (pageEndTime - pageStartTime).GetSeconds();
       
       if (pageTime > 0) {
         totalPageTime += pageTime;
         completedPageCount++;
         
         double pageTimeMs = pageTime * 1000.0;
         
         std::cout << "Page " << completedPageCount << " (" << page.requests.size() 
                   << " requests): " << pageTimeMs << " ms (" 
                   << pageCompletedRequests << "/" << page.requests.size() 
                   << " requests completed)"
                   << " - Total size: " << totalPageSize << " bytes"
                   << " - Completed size: " << completedPageSize << " bytes"
                   << std::endl;
       }
     }
     
     totalCompletedRequests += pageCompletedRequests;
   }
   
   if (completedPageCount > 0) {
     double avgPageTimeMs = (totalPageTime / completedPageCount) * 1000.0;
     std::cout << "\nAverage page load time: " << avgPageTimeMs << " ms" << std::endl;
     std::cout << "Completed " << completedPageCount << " out of " 
               << pages.size() << " pages (" 
               << (completedPageCount * 100.0 / pages.size()) << "%)" << std::endl;
   }
   
   if (totalCompletedRequests > 0) {
     std::cout << "Average request time: " << (totalRequestTime / totalCompletedRequests) 
               << " seconds" << std::endl;
     std::cout << "Completed " << totalCompletedRequests << " requests" << std::endl;
   }
   
   // Print flow monitoring statistics
   flowMonitor->CheckForLostPackets();
   Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());
   FlowMonitor::FlowStatsContainer stats = flowMonitor->GetFlowStats();
   
   std::cout << "\nFlow statistics:" << std::endl;
   std::cout << "------------------------------------" << std::endl;
   
   for (auto i = stats.begin(); i != stats.end(); ++i) {
     Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(i->first);
     
     std::cout << "Flow " << i->first << " (" << t.sourceAddress << ":" << t.sourcePort
               << " -> " << t.destinationAddress << ":" << t.destinationPort << ")" << std::endl;
     std::cout << "  Tx Packets: " << i->second.txPackets << std::endl;
     std::cout << "  Rx Packets: " << i->second.rxPackets << std::endl;
     
     if (i->second.timeLastRxPacket > i->second.timeFirstTxPacket) {
       double throughput = i->second.rxBytes * 8.0 / 
                          (i->second.timeLastRxPacket.GetSeconds() - 
                           i->second.timeFirstTxPacket.GetSeconds()) / 1000000;
       std::cout << "  Throughput: " << throughput << " Mbps" << std::endl;
     }
   }
   
   Simulator::Destroy();
   return 0;
 }