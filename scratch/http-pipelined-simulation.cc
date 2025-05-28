/* http-pipelined-simulation.cc
 * 
 * HTTP/1.1 pipelined mode simulation using UCB web trace data
 * OPTIMIZED version that addresses head-of-line blocking and connection usage
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
 
 NS_LOG_COMPONENT_DEFINE("HttpPipelinedSimulationOptimized");
 
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
 
 // Structure to track a pipelined connection - OPTIMIZED
 struct PipelinedConnection {
   Ptr<Socket> socket;
   std::queue<WebRequest*> pendingRequests;
   std::queue<WebRequest*> sentRequests;
   uint32_t pipelinedCount;
   bool isConnected;
   bool isConnecting;
   Address serverAddress;
   std::string receiveBuffer;
   uint32_t expectedBytes;        
   uint32_t receivedBytes;        
   bool inHeader;                 
   uint32_t totalPendingBytes;    // Track total bytes in pipeline
   
   PipelinedConnection() : socket(nullptr), pipelinedCount(0), isConnected(false), 
                          isConnecting(false), expectedBytes(0), receivedBytes(0), 
                          inHeader(true), totalPendingBytes(0) {}
 };
 
 // HTTP/1.1 pipelined client application - OPTIMIZED VERSION
 class HttpPipelinedClient : public Application {
 public:
   HttpPipelinedClient() : m_running(false), m_currentPageIndex(0), 
                          m_maxConnections(6), m_maxPipelineDepth(4), m_pageStartTime(Seconds(0)),
                          m_waitingForPrimary(true) {}
   virtual ~HttpPipelinedClient() {}
 
   static TypeId GetTypeId() {
     static TypeId tid = TypeId("ns3::HttpPipelinedClient")
       .SetParent<Application>()
       .SetGroupName("Applications")
       .AddConstructor<HttpPipelinedClient>();
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
     for (auto& conn : m_connections) {
       if (conn.socket) {
         conn.socket->Close();
         conn.socket = nullptr;
       }
     }
     Application::DoDispose();
   }
 
   virtual void StartApplication() {
     NS_LOG_FUNCTION(this);
     m_running = true;
     
     // OPTIMIZED: Use more connections like browsers do (6 instead of 2)
     m_connections.resize(m_maxConnections);
     for (auto& conn : m_connections) {
       conn.serverAddress = m_serverAddress;
     }
     
     ProcessNextPage();
   }
 
   virtual void StopApplication() {
     NS_LOG_FUNCTION(this);
     m_running = false;
     
     for (auto& conn : m_connections) {
       if (conn.socket) {
         conn.socket->Close();
         conn.socket = nullptr;
       }
     }
   }
 
 private:
   void ProcessNextPage() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       NS_LOG_INFO("Simulation complete - processed " << m_currentPageIndex << " pages");
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     
     if (page.requests.empty()) {
       NS_LOG_WARN("Empty page found at index " << m_currentPageIndex);
       page.isComplete = true;
       m_currentPageIndex++;
       Simulator::Schedule(MicroSeconds(1), &HttpPipelinedClient::ProcessNextPage, this);
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
     
     // Clear all connection states for new page
     for (auto& conn : m_connections) {
       while (!conn.pendingRequests.empty()) {
         conn.pendingRequests.pop();
       }
       while (!conn.sentRequests.empty()) {
         conn.sentRequests.pop();
       }
       conn.pipelinedCount = 0;
       conn.receiveBuffer.clear();
       conn.expectedBytes = 0;
       conn.receivedBytes = 0;
       conn.inHeader = true;
       conn.totalPendingBytes = 0;  // Reset pending bytes
     }
     
     NS_LOG_INFO("Starting page " << m_currentPageIndex << " with " << page.requests.size() << " requests");
     
     // Set a timeout for the entire page
     Simulator::Schedule(Seconds(30), &HttpPipelinedClient::HandlePageTimeout, this, m_currentPageIndex);
     
     StartPrimaryRequest();
   }
 
   void StartPrimaryRequest() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     if (page.requests.empty()) return;
     
     NS_LOG_INFO("Starting primary request for page " << m_currentPageIndex);
     
     // Use first connection for primary request
     PipelinedConnection& conn = m_connections[0];
     conn.pendingRequests.push(&page.requests[0]);
     
     ProcessConnection(conn);
   }
 
   // OPTIMIZED: Smart distribution of secondary requests
   void StartSecondaryRequests() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     if (page.requests.size() <= 1) return;
     
     NS_LOG_INFO("Starting " << (page.requests.size() - 1) << " secondary requests for page " << m_currentPageIndex);
     
     // OPTIMIZED: Sort secondary requests by size (smallest first to minimize HOL blocking)
     std::vector<WebRequest*> secondaryRequests;
     for (size_t i = 1; i < page.requests.size(); i++) {
       secondaryRequests.push_back(&page.requests[i]);
     }
     
     // Sort by size (ascending) to minimize head-of-line blocking
     std::sort(secondaryRequests.begin(), secondaryRequests.end(),
               [](WebRequest* a, WebRequest* b) { return a->size < b->size; });
     
     // OPTIMIZED: Distribute requests based on connection load
     for (auto* req : secondaryRequests) {
       // Find connection with least pending bytes (load balancing)
       size_t bestConnIndex = 0;
       uint32_t minPendingBytes = m_connections[0].totalPendingBytes;
       
       for (size_t i = 1; i < m_connections.size(); i++) {
         if (m_connections[i].totalPendingBytes < minPendingBytes) {
           minPendingBytes = m_connections[i].totalPendingBytes;
           bestConnIndex = i;
         }
       }
       
       PipelinedConnection& conn = m_connections[bestConnIndex];
       conn.pendingRequests.push(req);
       conn.totalPendingBytes += req->size;  // Track pending load
     }
     
     // Process all connections
     for (auto& conn : m_connections) {
       ProcessConnection(conn);
     }
   }
 
   void ProcessConnection(PipelinedConnection& conn) {
     if (!m_running) return;
     
     if (!conn.socket || !conn.isConnected) {
       if (!conn.isConnecting && !conn.pendingRequests.empty()) {
         ConnectToServer(conn);
       }
       return;
     }
     
     // OPTIMIZED: More aggressive pipelining for small requests
     uint32_t effectiveDepth = m_maxPipelineDepth;
     if (!conn.pendingRequests.empty() && conn.pendingRequests.front()->size < 1000) {
       effectiveDepth = m_maxPipelineDepth + 2;  // Allow more small requests
     }
     
     while (!conn.pendingRequests.empty() && 
            conn.pipelinedCount < effectiveDepth) {
       SendRequest(conn);
     }
   }
 
   void ConnectToServer(PipelinedConnection& conn) {
     NS_LOG_FUNCTION(this);
     
     conn.isConnecting = true;
     conn.socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
     conn.socket->Bind();
     
     // Find connection index for callbacks
     size_t connIndex = 0;
     for (size_t i = 0; i < m_connections.size(); i++) {
       if (&m_connections[i] == &conn) {
         connIndex = i;
         break;
       }
     }
     
     conn.socket->SetConnectCallback(
       MakeCallback(&HttpPipelinedClient::ConnectionSucceeded, this, connIndex),
       MakeCallback(&HttpPipelinedClient::ConnectionFailed, this, connIndex)
     );
     conn.socket->SetRecvCallback(
       MakeCallback(&HttpPipelinedClient::HandleRead, this, connIndex)
     );
     conn.socket->SetCloseCallbacks(
       MakeCallback(&HttpPipelinedClient::HandleClose, this, connIndex),
       MakeCallback(&HttpPipelinedClient::HandleClose, this, connIndex)
     );
     
     conn.socket->Connect(conn.serverAddress);
   }
 
   void ConnectionSucceeded(size_t connIndex, Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << connIndex);
     
     if (!m_running || connIndex >= m_connections.size()) return;
     
     PipelinedConnection& conn = m_connections[connIndex];
     conn.isConnected = true;
     conn.isConnecting = false;
     
     NS_LOG_INFO("Connection " << connIndex << " established");
     
     ProcessConnection(conn);
   }
 
   void ConnectionFailed(size_t connIndex, Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << connIndex);
     
     if (!m_running || connIndex >= m_connections.size()) return;
     
     PipelinedConnection& conn = m_connections[connIndex];
     conn.isConnecting = false;
     
     NS_LOG_ERROR("Connection " << connIndex << " failed");
     
     // OPTIMIZED: Better request redistribution
     if (!conn.pendingRequests.empty()) {
       NS_LOG_INFO("Redistributing " << conn.pendingRequests.size() << " pending requests");
       
       std::vector<WebRequest*> failedRequests;
       while (!conn.pendingRequests.empty()) {
         failedRequests.push_back(conn.pendingRequests.front());
         conn.pendingRequests.pop();
       }
       
       // Redistribute based on load
       for (auto* req : failedRequests) {
         size_t bestConnIndex = 0;
         uint32_t minPendingBytes = UINT32_MAX;
         
         for (size_t i = 0; i < m_connections.size(); i++) {
           if (i != connIndex && m_connections[i].totalPendingBytes < minPendingBytes) {
             minPendingBytes = m_connections[i].totalPendingBytes;
             bestConnIndex = i;
           }
         }
         
         if (bestConnIndex < m_connections.size()) {
           m_connections[bestConnIndex].pendingRequests.push(req);
           m_connections[bestConnIndex].totalPendingBytes += req->size;
           ProcessConnection(m_connections[bestConnIndex]);
         }
       }
     }
     
     if (conn.socket) {
       conn.socket->Close();
       conn.socket = nullptr;
     }
   }
 
   void SendRequest(PipelinedConnection& conn) {
     if (conn.pendingRequests.empty() || !conn.isConnected) {
       return;
     }
     
     WebRequest* req = conn.pendingRequests.front();
     conn.pendingRequests.pop();
     conn.sentRequests.push(req);
     conn.pipelinedCount++;
     
     req->startTime = Simulator::Now();
     
     // Extract path from URL
     std::string path = req->url;
     std::istringstream iss(req->url);
     std::string method, extractedPath, version;
     if (iss >> method >> extractedPath >> version) {
       path = extractedPath;
     }
     
     // Send HTTP/1.1 request with keep-alive
     std::ostringstream oss;
     oss << "GET " << path << "?size=" << req->size << " HTTP/1.1\r\n"
         << "Host: example.com\r\n"
         << "User-Agent: ns3-http-pipelined-client\r\n"
         << "Connection: keep-alive\r\n"
         << "\r\n";
     std::string request = oss.str();
     
     Ptr<Packet> packet = Create<Packet>((uint8_t*) request.c_str(), request.size());
     int result = conn.socket->Send(packet);
     
     if (result == -1) {
       NS_LOG_ERROR("Failed to send request for " << req->url);
     } else {
       NS_LOG_INFO("Sent pipelined request (pipeline depth: " << conn.pipelinedCount 
                   << ") for " << req->url << " (size=" << req->size << ")"
                   << (req->isPrimary ? " [PRIMARY]" : " [SECONDARY]"));
     }
   }
 
   void HandleRead(size_t connIndex, Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << connIndex);
     
     if (!m_running || connIndex >= m_connections.size()) return;
     
     PipelinedConnection& conn = m_connections[connIndex];
     
     Ptr<Packet> packet;
     Address from;
     
     while ((packet = socket->RecvFrom(from))) {
       uint32_t packetSize = packet->GetSize();
       uint8_t* buffer = new uint8_t[packetSize];
       packet->CopyData(buffer, packetSize);
       
       conn.receiveBuffer.append((char*)buffer, packetSize);
       delete[] buffer;
       
       ProcessResponses(conn);
     }
   }
 
   void ProcessResponses(PipelinedConnection& conn) {
     while (!conn.receiveBuffer.empty() && !conn.sentRequests.empty()) {
       
       if (conn.inHeader) {
         size_t headerEnd = conn.receiveBuffer.find("\r\n\r\n");
         if (headerEnd == std::string::npos) {
           break;
         }
         
         std::string headers = conn.receiveBuffer.substr(0, headerEnd);
         
         conn.expectedBytes = 0;
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
         
         conn.receiveBuffer.erase(0, headerEnd + 4);
         conn.receivedBytes = 0;
         conn.inHeader = false;
         
         NS_LOG_DEBUG("Parsed headers, expecting " << conn.expectedBytes << " bytes of content");
       }
       
       if (conn.receiveBuffer.length() >= conn.expectedBytes) {
         WebRequest* req = conn.sentRequests.front();
         conn.sentRequests.pop();
         req->completeTime = Simulator::Now();
         conn.pipelinedCount--;
         
         // OPTIMIZED: Update pending bytes tracking
         conn.totalPendingBytes = (conn.totalPendingBytes >= req->size) ? 
                                  conn.totalPendingBytes - req->size : 0;
         
         conn.receiveBuffer.erase(0, conn.expectedBytes);
         conn.inHeader = true;
         
         Time responseTime = req->completeTime - req->startTime;
         NS_LOG_INFO("Request completed in " << responseTime.GetSeconds() 
                     << " seconds (pipeline depth now: " << conn.pipelinedCount << ")"
                     << (req->isPrimary ? " [PRIMARY]" : " [SECONDARY]"));
         
         if (req->isPrimary) {
           HandlePrimaryRequestComplete();
         }
         
         ProcessConnection(conn);
         CheckPageComplete();
       } else {
         break;
       }
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
     
     uint32_t completedRequests = 0;
     for (const auto& req : page.requests) {
       if (!req.completeTime.IsZero()) {
         completedRequests++;
       }
     }
     
     if (completedRequests >= page.requests.size()) {
       page.isComplete = true;
       
       Time pageStartTime = Seconds(0);
       Time pageEndTime = Seconds(0);
       
       for (const auto& req : page.requests) {
         if (req.isPrimary && !req.startTime.IsZero()) {
           pageStartTime = req.startTime;
           break;
         }
       }
       
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
       Simulator::Schedule(MicroSeconds(10), &HttpPipelinedClient::ProcessNextPage, this);
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
     
     for (auto& req : page.requests) {
       if (req.completeTime.IsZero()) {
         req.completeTime = Simulator::Now();
       }
     }
     
     page.isComplete = true;
     m_currentPageIndex++;
     Simulator::Schedule(MicroSeconds(10), &HttpPipelinedClient::ProcessNextPage, this);
   }
 
   void HandleClose(size_t connIndex, Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << connIndex);
     
     if (connIndex >= m_connections.size()) return;
     
     PipelinedConnection& conn = m_connections[connIndex];
     conn.isConnected = false;
     conn.socket = nullptr;
     
     NS_LOG_INFO("Connection " << connIndex << " closed");
   }
 
   bool m_running;
   Address m_serverAddress;
   std::vector<WebPage> m_pages;
   uint32_t m_currentPageIndex;
   std::vector<PipelinedConnection> m_connections;
   uint32_t m_maxConnections;    // Now 6 connections
   uint32_t m_maxPipelineDepth;
   Time m_pageStartTime;
   bool m_waitingForPrimary;
 };

// Server implementation (same as before)
class HttpPipelinedServer : public Application {
public:
  HttpPipelinedServer() : m_socket(nullptr), m_running(false) {}
  virtual ~HttpPipelinedServer() {}

  static TypeId GetTypeId() {
    static TypeId tid = TypeId("ns3::HttpPipelinedServer")
      .SetParent<Application>()
      .SetGroupName("Applications")
      .AddConstructor<HttpPipelinedServer>();
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
        MakeCallback(&HttpPipelinedServer::HandleAccept, this)
      );
    }
    
    NS_LOG_INFO("HTTP/1.1 server listening on port " << m_port);
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
    
    socket->SetRecvCallback(MakeCallback(&HttpPipelinedServer::HandleRead, this));
    m_socketList.push_back(socket);
    m_socketBuffers[socket] = "";
    
    NS_LOG_INFO("Server accepted connection from " 
                << InetSocketAddress::ConvertFrom(from).GetIpv4());
  }

  void HandleRead(Ptr<Socket> socket) {
    NS_LOG_FUNCTION(this << socket);
    
    Ptr<Packet> packet;
    Address from;
    
    while ((packet = socket->RecvFrom(from))) {
      uint32_t packetSize = packet->GetSize();
      uint8_t* buffer = new uint8_t[packetSize];
      packet->CopyData(buffer, packetSize);
      
      m_socketBuffers[socket].append((char*)buffer, packetSize);
      delete[] buffer;
      
      ProcessRequests(socket);
    }
  }

  void ProcessRequests(Ptr<Socket> socket) {
    std::string& buffer = m_socketBuffers[socket];
    
    while (!buffer.empty()) {
      size_t requestEnd = buffer.find("\r\n\r\n");
      if (requestEnd == std::string::npos) {
        break;
      }
      
      std::string request = buffer.substr(0, requestEnd);
      buffer.erase(0, requestEnd + 4);
      
      NS_LOG_INFO("Server processing request");
      
      std::istringstream iss(request);
      std::string method, path, version;
      if (iss >> method >> path >> version) {
        SendResponse(socket, path);
      }
    }
  }

  void SendResponse(Ptr<Socket> socket, const std::string& url) {
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
    
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: text/html\r\n"
           << "Content-Length: " << responseSize << "\r\n"
           << "Connection: keep-alive\r\n"
           << "\r\n";
    std::string headerStr = header.str();
    
    Ptr<Packet> headerPacket = Create<Packet>((uint8_t*) headerStr.c_str(), headerStr.size());
    socket->Send(headerPacket);
    
    if (responseSize > 0) {
      uint32_t chunkSize = std::min(responseSize, (uint32_t)1400);
      uint32_t remaining = responseSize;
      
      while (remaining > 0) {
        uint32_t currentChunk = std::min(remaining, chunkSize);
        
        uint8_t* buffer = new uint8_t[currentChunk];
        memset(buffer, 'X', currentChunk);
        
        Ptr<Packet> dataPacket = Create<Packet>(buffer, currentChunk);
        socket->Send(dataPacket);
        
        delete[] buffer;
        remaining -= currentChunk;
      }
    }
    
    NS_LOG_INFO("Server sent response of " << responseSize << " bytes");
  }

  Ptr<Socket> m_socket;
  std::list<Ptr<Socket>> m_socketList;
  std::map<Ptr<Socket>, std::string> m_socketBuffers;
  uint16_t m_port;
  bool m_running;
};

// ReadTraceFile function (same as before)
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
  
  LogComponentEnable("HttpPipelinedSimulationOptimized", LOG_LEVEL_INFO);
  
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
  Ptr<HttpPipelinedServer> server = CreateObject<HttpPipelinedServer>();
  server->SetPort(port);
  nodes.Get(1)->AddApplication(server);
  server->SetStartTime(Seconds(1.0));
  server->SetStopTime(Seconds(simulationTime));
  
  // Create and install HTTP client
  Ptr<HttpPipelinedClient> client = CreateObject<HttpPipelinedClient>();
  Address serverAddress(InetSocketAddress(interfaces.GetAddress(1), port));
  client->SetServer(serverAddress);
  client->SetPages(pages);
  nodes.Get(0)->AddApplication(client);
  client->SetStartTime(Seconds(2.0));
  client->SetStopTime(Seconds(simulationTime));
  
  // Enable packet tracing
  AsciiTraceHelper ascii;
  pointToPoint.EnableAsciiAll(ascii.CreateFileStream("http-pipelined-simulation-optimized.tr"));
  pointToPoint.EnablePcapAll("http-pipelined-simulation-optimized");
  
  // Set up flow monitor
  Ptr<FlowMonitor> flowMonitor;
  FlowMonitorHelper flowHelper;
  flowMonitor = flowHelper.InstallAll();
  
  // Run simulation
  NS_LOG_INFO("Running HTTP/1.1 pipelined simulation (OPTIMIZED) for " << simulationTime << " seconds");
  Simulator::Stop(Seconds(simulationTime));
  Simulator::Run();
  
  // Process statistics
  std::cout << "Results for HTTP/1.1 pipelined mode (OPTIMIZED):" << std::endl;
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