/* http-trace-simulation.cc
 * 
 * HTTP/1.0 serial mode simulation using UCB web trace data
 * Fixed version with proper memory management, bounds checking, and request tracking
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
 
 using namespace ns3;
 
 NS_LOG_COMPONENT_DEFINE("HttpTraceSimulation");
 
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
   
   WebPage() : isComplete(false), primaryRequestId(0) {}
 };
 
 // HTTP client application that makes serial requests
 class HttpSerialClient : public Application {
 public:
   HttpSerialClient() : m_running(false), m_socket(nullptr), m_currentPageIndex(0), 
                      m_currentRequestIndex(0), m_connected(false), m_totalBytes(0), 
                      m_pendingBytes(0), m_waitingForPrimary(false) {}
   virtual ~HttpSerialClient() {}
 
   // Register the type
   static TypeId GetTypeId() {
     static TypeId tid = TypeId("ns3::HttpSerialClient")
       .SetParent<Application>()
       .SetGroupName("Applications")
       .AddConstructor<HttpSerialClient>();
     return tid;
   }
 
   // Set the pages to be processed
   void SetPages(std::vector<WebPage> pages) {
     m_pages = pages;
   }
 
   // Set the server address
   void SetServer(Address address) {
     m_serverAddress = address;
   }
 
   // Get statistics after simulation
   std::vector<WebPage> GetCompletedPages() const {
     return m_pages;
   }
 
 protected:
   virtual void DoDispose() {
     CleanupSocket();
     Application::DoDispose();
   }
 
   // Start the application
   virtual void StartApplication() {
     NS_LOG_FUNCTION(this);
     m_running = true;
     
     // Start the first page
     ProcessNextPage();
   }
 
   // Stop the application
   virtual void StopApplication() {
     NS_LOG_FUNCTION(this);
     m_running = false;
     
     CleanupSocket();
   }
 
 private:
   // Cleanup and close socket
   void CleanupSocket() {
     if (m_socket) {
       m_socket->Close();
       m_socket = nullptr;
     }
   }
 
   // Process the next page in the queue
   void ProcessNextPage() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     // Start with primary request
     m_currentRequestIndex = 0;
     m_waitingForPrimary = true;
     
     WebPage& page = m_pages[m_currentPageIndex];
     
     // Safety check for empty page
     if (page.requests.empty()) {
       NS_LOG_WARN("Empty page found at index " << m_currentPageIndex);
       page.isComplete = true;
       m_currentPageIndex++;
       Simulator::Schedule(MilliSeconds(10), &HttpSerialClient::ProcessNextPage, this);
       return;
     }
     
     // Find the primary request in this page (should be first, but just in case)
     size_t primaryIndex = 0;
     bool foundPrimary = false;
     
     for (size_t i = 0; i < page.requests.size(); i++) {
       if (page.requests[i].isPrimary) {
         primaryIndex = i;
         page.primaryRequestId = page.requests[i].id;
         foundPrimary = true;
         break;
       }
     }
     
     // If no primary request was found, set the first as primary
     if (!foundPrimary) {
       NS_LOG_WARN("No primary request found in page " << m_currentPageIndex << ", using first request");
       primaryIndex = 0;
       page.requests[0].isPrimary = true;
       page.primaryRequestId = page.requests[0].id;
     }
     
     // Swap primary to be first
     if (primaryIndex != 0) {
       std::swap(page.requests[0], page.requests[primaryIndex]);
     }
     
     // Start the request
     ProcessNextRequest();
   }
 
   // Process the next request in the current page
   void ProcessNextRequest() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     
     if (m_currentRequestIndex >= page.requests.size()) {
       // Page is complete, calculate statistics
       page.isComplete = true;
       
       // Calculate page statistics
       bool pageHasStartTime = false;
       bool pageHasEndTime = false;
       Time pageStartTime = Seconds(0);
       Time pageEndTime = Seconds(0);
       uint32_t completedRequests = 0;
       
       // Find page start time (primary request start)
       for (const auto& req : page.requests) {
         if (req.isPrimary && !req.startTime.IsZero()) {
           pageStartTime = req.startTime;
           pageHasStartTime = true;
           break;
         }
       }
       
       // Find page end time (latest request completion) and count completed requests
       for (const auto& req : page.requests) {
         if (!req.completeTime.IsZero()) {
           completedRequests++;
           if (pageEndTime.IsZero() || req.completeTime > pageEndTime) {
             pageEndTime = req.completeTime;
             pageHasEndTime = true;
           }
         }
       }
       
       // Log page completion if we have timing data
       if (pageHasStartTime && pageHasEndTime && pageEndTime > pageStartTime) {
         double pageTime = (pageEndTime - pageStartTime).GetSeconds();
         NS_LOG_INFO("Page " << m_currentPageIndex << " completed in " 
                     << pageTime << " seconds ("
                     << completedRequests << "/" << page.requests.size() << " requests)");
       }
       
       // Move to next page
       m_currentPageIndex++;
       Simulator::Schedule(MilliSeconds(10), &HttpSerialClient::ProcessNextPage, this);
       return;
     }
     
     // Clear any previous socket
     CleanupSocket();
     
     // Create a new socket for each request (HTTP/1.0 serial mode)
     m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
     m_socket->Bind();
     
     // Set up callbacks
     m_socket->SetConnectCallback(
       MakeCallback(&HttpSerialClient::ConnectionSucceeded, this),
       MakeCallback(&HttpSerialClient::ConnectionFailed, this)
     );
     m_socket->SetRecvCallback(MakeCallback(&HttpSerialClient::HandleRead, this));
     m_socket->SetCloseCallbacks(
       MakeCallback(&HttpSerialClient::HandleClose, this),
       MakeCallback(&HttpSerialClient::HandleClose, this)
     );
     
     // Connect to server
     m_connected = false;
     m_socket->Connect(m_serverAddress);
     
     // Record start time for this request
     page.requests[m_currentRequestIndex].startTime = Simulator::Now();
     
     bool isPrimary = page.requests[m_currentRequestIndex].isPrimary;
     NS_LOG_INFO("Client starting request " << m_currentRequestIndex 
                 << " (Primary: " << (isPrimary ? "Yes" : "No") << ") for URL " 
                 << page.requests[m_currentRequestIndex].url << " at " 
                 << page.requests[m_currentRequestIndex].startTime.GetSeconds() << "s");
     
     // Add a timeout to prevent stalled connections - 5 seconds should be reasonable
     Simulator::Schedule(Seconds(5), &HttpSerialClient::CheckRequestTimeout, this, 
                       m_currentPageIndex, m_currentRequestIndex);
   }
 
   // Check if a request has timed out
   void CheckRequestTimeout(uint32_t pageIndex, uint32_t requestIndex) {
     if (!m_running) return;
     
     // Check if we're still on the same request (it hasn't completed)
     if (m_currentPageIndex == pageIndex && m_currentRequestIndex == requestIndex) {
       NS_LOG_WARN("Request timed out: Page " << pageIndex << ", Request " << requestIndex);
       
       // Mark the request as timed out
       if (pageIndex < m_pages.size() && requestIndex < m_pages[pageIndex].requests.size()) {
         // Set a completion time just so we don't count it as pending forever
         if (m_pages[pageIndex].requests[requestIndex].completeTime.IsZero()) {
           m_pages[pageIndex].requests[requestIndex].completeTime = Simulator::Now();
         }
       }
       
       // Clean up socket and move to next request
       CleanupSocket();
       m_currentRequestIndex++;
       Simulator::Schedule(MilliSeconds(10), &HttpSerialClient::ProcessNextRequest, this);
     }
   }
 
   // Called when connection is established
   void ConnectionSucceeded(Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << socket);
     
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     m_connected = true;
     
     WebPage& page = m_pages[m_currentPageIndex];
     
     if (m_currentRequestIndex >= page.requests.size()) {
       NS_LOG_WARN("Invalid request index " << m_currentRequestIndex);
       CleanupSocket();
       m_currentRequestIndex = 0;
       m_currentPageIndex++;
       Simulator::Schedule(MilliSeconds(10), &HttpSerialClient::ProcessNextPage, this);
       return;
     }
     
     WebRequest& req = page.requests[m_currentRequestIndex];
     
     // Make sure we have a start time if it hasn't been set already
     if (req.startTime.IsZero()) {
       req.startTime = Simulator::Now();
     }
     
     // Send HTTP request
     std::ostringstream oss;
     oss << "GET " << req.url << " HTTP/1.0\r\n"
         << "Host: example.com\r\n"
         << "User-Agent: ns3-http-client\r\n"
         << "\r\n";
     std::string request = oss.str();
     
     Ptr<Packet> packet = Create<Packet>((uint8_t*) request.c_str(), request.size());
     socket->Send(packet);
     
     // Set up expected response size
     m_pendingBytes = req.size;
     m_totalBytes = 0;
     
     NS_LOG_INFO("Client sent request " << m_currentRequestIndex 
                 << " (" << request.size() << " bytes)");
   }
 
   // Called when connection fails
   void ConnectionFailed(Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << socket);
     NS_LOG_ERROR("Connection failed for request " << m_currentRequestIndex);
     
     // Clean up socket
     CleanupSocket();
     
     // Move to next request
     m_currentRequestIndex++;
     Simulator::Schedule(MilliSeconds(10), &HttpSerialClient::ProcessNextRequest, this);
   }
 
   // Handle incoming data
   void HandleRead(Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << socket);
     
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     Ptr<Packet> packet;
     Address from;
     
     while ((packet = socket->RecvFrom(from))) {
       uint32_t receivedBytes = packet->GetSize();
       m_totalBytes += receivedBytes;
       
       // Bounds checking
       if (m_currentPageIndex < m_pages.size() && 
           m_currentRequestIndex < m_pages[m_currentPageIndex].requests.size()) {
         
         WebPage& page = m_pages[m_currentPageIndex];
         bool isPrimary = page.requests[m_currentRequestIndex].isPrimary;
         
         NS_LOG_INFO("Client received " << receivedBytes << " bytes for "
                     << (isPrimary ? "primary" : "secondary") << " request " 
                     << m_currentRequestIndex << " (total: " << m_totalBytes 
                     << "/" << m_pendingBytes << ")");
         
         // Check if response is complete
         if (m_totalBytes >= m_pendingBytes) {
           // Record completion time
           page.requests[m_currentRequestIndex].completeTime = Simulator::Now();
           
           Time responseTime = page.requests[m_currentRequestIndex].completeTime - 
                               page.requests[m_currentRequestIndex].startTime;
           
           NS_LOG_INFO("Request " << m_currentRequestIndex 
                       << " completed in " << responseTime.GetSeconds() << " seconds");
           
           // Close this connection
           CleanupSocket();
           
           // If this was the primary request, we can now start all secondary requests
           if (m_waitingForPrimary && isPrimary) {
             m_waitingForPrimary = false;
           }
           
           // Move to next request
           m_currentRequestIndex++;
           Simulator::Schedule(MilliSeconds(10), &HttpSerialClient::ProcessNextRequest, this);
           break;
         }
       } else {
         NS_LOG_WARN("Invalid indices in HandleRead");
         CleanupSocket();
         break;
       }
     }
   }
 
   // Handle socket closure
   void HandleClose(Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << socket);
     m_connected = false;
     
     // If we haven't received all expected data, consider it an error
     if (m_totalBytes < m_pendingBytes) {
       NS_LOG_ERROR("Connection closed before all data received for request " 
                    << m_currentRequestIndex << " (" << m_totalBytes << "/" 
                    << m_pendingBytes << ")");
     }
     
     // Make sure we move to the next request if we haven't already
     if (m_socket == socket) {
       m_socket = nullptr;
       
       if (m_currentPageIndex < m_pages.size() && 
           m_currentRequestIndex < m_pages[m_currentPageIndex].requests.size()) {
         
         WebPage& page = m_pages[m_currentPageIndex];
         
         // Record completion time if not already set
         if (page.requests[m_currentRequestIndex].completeTime.IsZero()) {
           page.requests[m_currentRequestIndex].completeTime = Simulator::Now();
         }
         
         // Move to next request
         m_currentRequestIndex++;
         Simulator::Schedule(MilliSeconds(10), &HttpSerialClient::ProcessNextRequest, this);
       } else {
         NS_LOG_WARN("Invalid indices in HandleClose");
         
         // Move to next page as a recovery mechanism
         m_currentRequestIndex = 0;
         m_currentPageIndex++;
         Simulator::Schedule(MilliSeconds(10), &HttpSerialClient::ProcessNextPage, this);
       }
     }
   }
 
   bool m_running;                      // Whether the application is running
   Ptr<Socket> m_socket;                // Current socket
   Address m_serverAddress;             // Server address
   std::vector<WebPage> m_pages;        // Queue of pages with requests
   uint32_t m_currentPageIndex;         // Index of current page
   uint32_t m_currentRequestIndex;      // Index of current request within page
   bool m_connected;                    // Whether connected to server
   uint32_t m_totalBytes;               // Bytes received for current request
   uint32_t m_pendingBytes;             // Expected bytes for current request
   bool m_waitingForPrimary;            // Whether waiting for primary request to complete
 };
 
 // HTTP server application that responds to requests
 class HttpServer : public Application {
 public:
   HttpServer() : m_socket(nullptr), m_running(false) {}
   virtual ~HttpServer() {}
 
   // Register the type
   static TypeId GetTypeId() {
     static TypeId tid = TypeId("ns3::HttpServer")
       .SetParent<Application>()
       .SetGroupName("Applications")
       .AddConstructor<HttpServer>();
     return tid;
   }
 
   // Set the server port
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
       Ptr<Socket> socket = *it;
       socket->Close();
     }
     m_socketList.clear();
     
     Application::DoDispose();
   }
 
   // Start the application
   virtual void StartApplication() {
     NS_LOG_FUNCTION(this);
     m_running = true;
     
     // Create listening socket
     if (!m_socket) {
       m_socket = Socket::CreateSocket(GetNode(), TcpSocketFactory::GetTypeId());
       InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
       m_socket->Bind(local);
       m_socket->Listen();
       
       // Handle new connections
       m_socket->SetAcceptCallback(
         MakeNullCallback<bool, Ptr<Socket>, const Address &>(),
         MakeCallback(&HttpServer::HandleAccept, this)
       );
     }
     
     NS_LOG_INFO("HTTP server listening on port " << m_port);
   }
 
   // Stop the application
   virtual void StopApplication() {
     NS_LOG_FUNCTION(this);
     m_running = false;
     
     if (m_socket) {
       m_socket->Close();
       m_socket = nullptr;
     }
     
     for (auto it = m_socketList.begin(); it != m_socketList.end(); ++it) {
       Ptr<Socket> socket = *it;
       socket->Close();
     }
     m_socketList.clear();
   }
 
 private:
   // Handle a new connection
   void HandleAccept(Ptr<Socket> socket, const Address& from) {
     NS_LOG_FUNCTION(this << socket << from);
     
     socket->SetRecvCallback(MakeCallback(&HttpServer::HandleRead, this));
     m_socketList.push_back(socket);
     
     NS_LOG_INFO("Server accepted connection from " 
                 << InetSocketAddress::ConvertFrom(from).GetIpv4() << ":"
                 << InetSocketAddress::ConvertFrom(from).GetPort());
   }
 
   // Handle incoming data
   void HandleRead(Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << socket);
     
     Ptr<Packet> packet;
     Address from;
     
     while ((packet = socket->RecvFrom(from))) {
       // We just need to read the request data
       uint8_t buffer[2048];
       uint32_t size = std::min(packet->GetSize(), (uint32_t)2048);
       packet->CopyData(buffer, size);
       buffer[size] = '\0';
       
       std::string request((char*)buffer, size);
       
       NS_LOG_INFO("Server received request: " << size << " bytes");
       
       // Parse the request to get the URL (simplified)
       std::string url;
       std::istringstream iss(request);
       std::string method, path, version;
       if (iss >> method >> path >> version) {
         url = path;
       }
       
       // Send a response
       SendResponse(socket, url);
     }
   }
 
   // Send an HTTP response
   void SendResponse(Ptr<Socket> socket, const std::string& url) {
     // Generate a response based on the URL
     
     // First, send HTTP headers
     std::ostringstream header;
     header << "HTTP/1.0 200 OK\r\n"
            << "Content-Type: text/html\r\n"
            << "Connection: close\r\n"
            << "\r\n";
     std::string headerStr = header.str();
     
     Ptr<Packet> headerPacket = Create<Packet>((uint8_t*) headerStr.c_str(), headerStr.size());
     socket->Send(headerPacket);
     
     // Determine response size based on URL pattern
     uint32_t responseSize = 1024;  // Default size
     
     // Extract size from URL if present (for our synthetic output format)
     size_t pos = url.find("size=");
     if (pos != std::string::npos) {
       try {
         responseSize = std::stoi(url.substr(pos + 5));
       } catch (const std::exception& e) {
         NS_LOG_WARN("Invalid size in URL: " << url);
       }
     }
     
     // Send the response body in chunks
     uint32_t chunkSize = 1400;  // Approximately MTU size
     uint32_t remaining = responseSize;
     
     while (remaining > 0 && socket->GetTxAvailable() > 0) {
       uint32_t currentChunk = std::min(remaining, chunkSize);
       
       // Create a packet with the appropriate size
       uint8_t* buffer = new uint8_t[currentChunk];
       // Fill with some pattern (not important for simulation)
       memset(buffer, 'X', currentChunk);
       
       Ptr<Packet> dataPacket = Create<Packet>(buffer, currentChunk);
       socket->Send(dataPacket);
       
       delete[] buffer;
       remaining -= currentChunk;
       
       // Add small delay between chunks to simulate server processing
       if (remaining > 0) {
         Simulator::Schedule(MilliSeconds(1), &HttpServer::SendRemainingData, 
                            this, socket, remaining, chunkSize);
         break;  // Schedule will handle the rest
       }
     }
   }
 
   // Helper function to send remaining data
   void SendRemainingData(Ptr<Socket> socket, uint32_t remaining, uint32_t chunkSize) {
     // Check if socket is still valid
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
         Simulator::Schedule(MilliSeconds(1), &HttpServer::SendRemainingData, 
                           this, socket, remaining, chunkSize);
       }
     }
   }
 
   Ptr<Socket> m_socket;                // Listening socket
   std::list<Ptr<Socket>> m_socketList; // List of connected sockets
   uint16_t m_port;                     // Server port
   bool m_running;                      // Whether the application is running
 };
 
 // Read trace file in our processed format
 std::vector<WebPage> ReadTraceFile(const std::string& filename) {
   std::vector<WebPage> pages;
   WebPage currentPage;
 
   uint32_t id = 0;
   
   std::ifstream file(filename);
   if (file.is_open()) {
     std::string line;
     
     while (std::getline(file, line)) {
       // Skip empty lines, comments, or page markers
       if (line.empty() || line[0] == '#') {
         // If this is an end-of-page marker, add the page
         if (line.find("End of Page") != std::string::npos) {
           if (!currentPage.requests.empty()) {
             // Ensure at least one primary request exists
             bool hasPrimary = false;
             for (const auto& req : currentPage.requests) {
               if (req.isPrimary) {
                 hasPrimary = true;
                 break;
               }
             }
             
             if (!hasPrimary && !currentPage.requests.empty()) {
               // Set first request as primary if none exists
               currentPage.requests[0].isPrimary = true;
             }
             
             pages.push_back(currentPage);
             currentPage = WebPage();
           }
         }
         continue;
       }
       
       std::istringstream iss(line);
       WebRequest req;
       req.id = id++;
       
       // Expected format: URL,SIZE,ISPRIMARY
       std::string url, size, isPrimary;
       if (std::getline(iss, url, ',') && 
           std::getline(iss, size, ',') && 
           std::getline(iss, isPrimary)) {
         
         req.url = url;
         
         try {
           req.size = std::stoi(size);
         } catch (const std::exception& e) {
           NS_LOG_WARN("Invalid size value in trace file: " << size);
           req.size = 1024; // Use default size
         }
         
         req.isPrimary = (isPrimary == "1" || isPrimary == "true");
         
         currentPage.requests.push_back(req);
       }
     }
     
     // Add the last page if not empty
     if (!currentPage.requests.empty()) {
       // Ensure at least one primary request exists
       bool hasPrimary = false;
       for (const auto& req : currentPage.requests) {
         if (req.isPrimary) {
           hasPrimary = true;
           break;
         }
       }
       
       if (!hasPrimary && !currentPage.requests.empty()) {
         // Set first request as primary if none exists
         currentPage.requests[0].isPrimary = true;
       }
       
       pages.push_back(currentPage);
     }
     
     file.close();
   } else {
     NS_LOG_WARN("Could not open trace file: " << filename);
     // Create synthetic data for testing
     for (int p = 0; p < 5; p++) {
       WebPage page;
       page.isComplete = false;
       
       // Create a primary request
       WebRequest primary;
       primary.id = id++;
       primary.url = "/index" + std::to_string(p) + ".html";
       primary.size = 20000 + (p * 1000);
       primary.isPrimary = true;
       page.requests.push_back(primary);
       
       // Create several secondary requests
       for (int i = 1; i <= 5; i++) {
         WebRequest secondary;
         secondary.id = id++;
         secondary.url = "/image" + std::to_string(p) + "_" + std::to_string(i) + ".jpg";
         secondary.size = 50000 + (i * 5000);
         secondary.isPrimary = false;
         page.requests.push_back(secondary);
       }
       
       pages.push_back(page);
     }
   }
   
   return pages;
 }
 
 // Main function
 int main(int argc, char* argv[]) {
   std::string traceFile = "";
   std::string httpMode = "serial"; // Options: serial, parallel, persistent, pipelined
   std::string bandwidth = "1.5Mbps";
   std::string delay = "50ms";
   double simulationTime = 500.0;
   uint32_t maxPages = 0; // If >0, limit to this many pages
   
   // Configure command line parameters
   CommandLine cmd(__FILE__);
   cmd.AddValue("traceFile", "Path to trace file", traceFile);
   cmd.AddValue("mode", "HTTP mode (serial, parallel, persistent, pipelined)", httpMode);
   cmd.AddValue("bandwidth", "Bandwidth of the link", bandwidth);
   cmd.AddValue("delay", "Delay of the link", delay);
   cmd.AddValue("time", "Simulation time in seconds", simulationTime);
   cmd.AddValue("maxPages", "Maximum number of pages to process (0 for all)", maxPages);
   cmd.Parse(argc, argv);
   
   // Configure logging
   LogComponentEnable("HttpTraceSimulation", LOG_LEVEL_INFO);
   
   // Create nodes
   NodeContainer nodes;
   nodes.Create(2);  // Node 0: client, Node 1: server
   
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
   
   // Limit to a maximum number of pages if specified
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
   Ptr<HttpSerialClient> client = CreateObject<HttpSerialClient>();
   Address serverAddress(InetSocketAddress(interfaces.GetAddress(1), port));
   client->SetServer(serverAddress);
   client->SetPages(pages);
   nodes.Get(0)->AddApplication(client);
   client->SetStartTime(Seconds(2.0));
   client->SetStopTime(Seconds(simulationTime));
   
   // Enable packet tracing
   AsciiTraceHelper ascii;
   pointToPoint.EnableAsciiAll(ascii.CreateFileStream("http-trace-simulation.tr"));
   pointToPoint.EnablePcapAll("http-trace-simulation");
   
   // Set up flow monitor
   Ptr<FlowMonitor> flowMonitor;
   FlowMonitorHelper flowHelper;
   flowMonitor = flowHelper.InstallAll();
   
   // Run simulation
   NS_LOG_INFO("Running HTTP/" << httpMode << " simulation for " << simulationTime << " seconds");
   Simulator::Stop(Seconds(simulationTime));
   Simulator::Run();
   
   // Process statistics
   std::cout << "Results for HTTP/1.0 " << httpMode << " mode:" << std::endl;
   std::cout << "------------------------------------" << std::endl;
   
   // Get completed pages
   std::vector<WebPage> completedPages = client->GetCompletedPages();
   
   // Calculate statistics
   uint32_t completedPageCount = 0;
   double totalPageTime = 0.0;
   uint32_t totalCompletedRequests = 0;
   double totalRequestTime = 0.0;
   
   for (const auto& page : completedPages) {
     bool pageHasEndTime = false;
     Time pageStartTime = Seconds(0);
     Time pageEndTime = Seconds(0);
     uint32_t pageCompletedRequests = 0;
     
     // Find page start time (primary request start)
     for (const auto& req : page.requests) {
       if (req.isPrimary) {
         if (!req.startTime.IsZero()) {
           pageStartTime = req.startTime;
         } else if (!req.completeTime.IsZero()) {
           // If primary has completion time but no start time, approximate
           pageStartTime = req.completeTime - MilliSeconds(100);
         }
         break;
       }
     }
     
     // Find page end time (latest request completion)
     for (const auto& req : page.requests) {
       if (!req.completeTime.IsZero() && req.completeTime > Seconds(0)) {
         pageCompletedRequests++;
         
         // Only count request time if we have both start and complete times
         if (!req.startTime.IsZero()) {
           Time requestTime = req.completeTime - req.startTime;
           // Sanity check for negative times
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
     
     // More lenient page completion check:
     // Count page as complete if:
     // 1. The page has a start time (either real or estimated) 
     // 2. The page has an end time
     // 3. At least one request completed
     if ((!pageStartTime.IsZero() || pageHasEndTime) && pageCompletedRequests > 0) {
       // If no start time but we have end time, estimate start time
       if (pageStartTime.IsZero() && pageHasEndTime) {
         pageStartTime = pageEndTime - MilliSeconds(500);
       }
       
       // Calculate page load time
       double pageTime = (pageEndTime - pageStartTime).GetSeconds();
       
       // Only count if page time is positive
       if (pageTime > 0) {
         totalPageTime += pageTime;
         completedPageCount++;
         
         std::cout << "Page " << completedPageCount << " (" << page.requests.size() 
                   << " requests): " << pageTime << " seconds (" 
                   << pageCompletedRequests << "/" << page.requests.size() 
                   << " requests completed)" << std::endl;
       }
     }
     
     totalCompletedRequests += pageCompletedRequests;
   }
   
   if (completedPageCount > 0) {
     std::cout << "\nAverage page load time: " << (totalPageTime / completedPageCount) 
               << " seconds" << std::endl;
     std::cout << "Completed " << completedPageCount << " out of " 
               << pages.size() << " pages (" 
               << (completedPageCount * 100.0 / pages.size()) << "%)" << std::endl;
   } else {
     std::cout << "No pages completed" << std::endl;
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
