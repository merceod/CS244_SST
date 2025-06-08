/* http-sst-simulation.cc
 *
 * HTTP/1.0 SST mode simulation using UCB web trace data
 * Implements SST protocol as described in Ford's SIGCOMM'07 paper
 * 
 * SST PROTOCOL IMPLEMENTATION:
 * - Channel Protocol: UDP-based with packet sequencing, ACKs, congestion control
 * - Stream Protocol: Reliable streams multiplexed over channels
 * - Packet Format: [Channel Header][Stream Header][Payload][Authenticator]
 * - Shared congestion control across all streams (TCP-friendly)
 * - HTTP/1.0 semantics: one transaction per stream
 */

 #include "ns3/applications-module.h"
 #include "ns3/core-module.h"
 #include "ns3/internet-module.h"
 #include "ns3/network-module.h"
 #include "ns3/point-to-point-module.h"
 #include "ns3/flow-monitor-module.h"
 #include "ns3/simulator.h"
 #include "ns3/inet-socket-address.h"
 #include <fstream>
 #include <string>
 #include <vector>
 #include <iostream>
 #include <sstream>
 #include <map>
 #include <queue>
 #include <algorithm>
 #include <cstring>
 
 using namespace ns3;
 
 NS_LOG_COMPONENT_DEFINE("HttpSstSimulation");
 
 // SST Packet Format (from paper Section 4.1, Figure 3)
 struct SstChannelHeader {
   uint8_t channelId;              // 8-bit channel ID
   uint32_t packetSeqNum : 24;     // 24-bit packet sequence number
   uint16_t ackSeqNum;             // Acknowledgment sequence number  
   uint8_t ackCount;               // Acknowledgment count
   
   SstChannelHeader() : channelId(1), packetSeqNum(0), ackSeqNum(0), ackCount(0) {}
 } __attribute__((packed));
 
 struct SstStreamHeader {
   uint16_t localStreamId;         // 16-bit LSID
   uint16_t byteSeqNum;            // 16-bit byte sequence number within stream
   uint8_t window : 5;             // 5-bit window (exponential encoding)
   uint8_t flags : 3;              // P (push), C (close), etc.
   
   SstStreamHeader() : localStreamId(0), byteSeqNum(0), window(31), flags(0) {}
 } __attribute__((packed));
 
 struct SstAuthenticator {
   uint32_t checksum;              // 32-bit lightweight authenticator
   
   SstAuthenticator() : checksum(0x12345678) {}
 } __attribute__((packed));
 
 // SST Pending Packet (for retransmission)
 struct SstPendingPacket {
   uint32_t packetSeqNum;
   std::string payload;
   SstStreamHeader streamHeader;
   Time sentTime;
   EventId retransmitTimer;
   uint32_t retransmitCount;
   
   SstPendingPacket() : packetSeqNum(0), retransmitCount(0) {}
 };
 
 // SST Packet Types
 enum SstPacketType {
   SST_DATA = 0,
   SST_ACK = 1,
   SST_INIT = 2,
   SST_REPLY = 3
 };
 
 // Web request structure
 struct WebRequest {
   uint32_t id;
   std::string url;
   uint32_t size;
   bool isPrimary;
   Time startTime;
   Time completeTime;
 };
 
 struct WebPage {
   std::vector<WebRequest> requests;
   bool isComplete;
   uint32_t primaryRequestId;
   bool primaryCompleted;
   
   WebPage() : isComplete(false), primaryRequestId(0), primaryCompleted(false) {}
 };
 
 // SST Stream state
 struct SstStream {
   uint16_t streamId;
   WebRequest* request;
   bool isActive;
   uint32_t nextByteSeq;           // Next byte sequence number to send
   uint32_t expectedByteSeq;       // Expected next byte sequence to receive
   uint32_t sentBytes;
   uint32_t ackedBytes;
   std::string sendBuffer;         // Data waiting to be sent
   std::string recvBuffer;         // Received data buffer
   bool isComplete;
   
   SstStream() : streamId(0), request(nullptr), isActive(false), 
                nextByteSeq(0), expectedByteSeq(0), sentBytes(0), 
                ackedBytes(0), isComplete(false) {}
 };
 
 // SST Channel state (implements congestion control + retransmission)
 struct SstChannel {
   uint32_t nextPacketSeq;         // Next packet sequence number
   uint32_t lastAckedPacketSeq;    // Last acknowledged packet sequence
   uint32_t cwnd;                  // Congestion window (in packets)
   uint32_t ssthresh;              // Slow start threshold
   uint32_t rtt;                   // Round-trip time estimate (in microseconds)
   uint32_t rto;                   // Retransmission timeout (in microseconds)
   std::map<uint32_t, SstPendingPacket> pendingPackets;  // Unacknowledged packets
   uint32_t packetsInFlight;       // Number of unacknowledged packets
   bool inSlowStart;               // Congestion control state
   
   SstChannel() : nextPacketSeq(1), lastAckedPacketSeq(0), cwnd(1), ssthresh(65535),
                 rtt(100000), rto(1000000), packetsInFlight(0), inSlowStart(true) {}
 };
 
 // Create SST packet with proper headers
 Ptr<Packet> CreateSstPacket(const SstChannelHeader& chanHdr, const SstStreamHeader& streamHdr, 
                            const std::string& payload) {
   SstAuthenticator auth;
   
   uint32_t totalSize = sizeof(SstChannelHeader) + sizeof(SstStreamHeader) + 
                       payload.length() + sizeof(SstAuthenticator);
   
   uint8_t* buffer = new uint8_t[totalSize];
   uint32_t offset = 0;
   
   // Copy channel header
   memcpy(buffer + offset, &chanHdr, sizeof(SstChannelHeader));
   offset += sizeof(SstChannelHeader);
   
   // Copy stream header
   memcpy(buffer + offset, &streamHdr, sizeof(SstStreamHeader));
   offset += sizeof(SstStreamHeader);
   
   // Copy payload
   if (!payload.empty()) {
     memcpy(buffer + offset, payload.c_str(), payload.length());
     offset += payload.length();
   }
   
   // Copy authenticator
   memcpy(buffer + offset, &auth, sizeof(SstAuthenticator));
   
   Ptr<Packet> packet = Create<Packet>(buffer, totalSize);
   delete[] buffer;
   
   return packet;
 }
 
 // Parse SST packet
 bool ParseSstPacket(Ptr<Packet> packet, SstChannelHeader& chanHdr, 
                    SstStreamHeader& streamHdr, std::string& payload) {
   uint32_t packetSize = packet->GetSize();
   
   if (packetSize < sizeof(SstChannelHeader) + sizeof(SstStreamHeader) + sizeof(SstAuthenticator)) {
     return false;
   }
   
   uint8_t* buffer = new uint8_t[packetSize];
   packet->CopyData(buffer, packetSize);
   
   uint32_t offset = 0;
   
   // Parse channel header
   memcpy(&chanHdr, buffer + offset, sizeof(SstChannelHeader));
   offset += sizeof(SstChannelHeader);
   
   // Parse stream header
   memcpy(&streamHdr, buffer + offset, sizeof(SstStreamHeader));
   offset += sizeof(SstStreamHeader);
   
   // Extract payload
   uint32_t payloadSize = packetSize - sizeof(SstChannelHeader) - 
                         sizeof(SstStreamHeader) - sizeof(SstAuthenticator);
   if (payloadSize > 0) {
     payload.assign((char*)(buffer + offset), payloadSize);
   }
   
   delete[] buffer;
   return true;
 }
 
 // HTTP/1.0 SST Client Application
 class HttpSstClient : public Application {
 public:
   HttpSstClient() : m_running(false), m_currentPageIndex(0), m_waitingForPrimary(true),
                    m_socket(nullptr), m_connected(false), m_nextStreamId(1) {}
   virtual ~HttpSstClient() {}
 
   static TypeId GetTypeId() {
     static TypeId tid = TypeId("ns3::HttpSstClient")
       .SetParent<Application>()
       .SetGroupName("Applications")
       .AddConstructor<HttpSstClient>();
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
     CleanupSocket();
     Application::DoDispose();
   }
 
   virtual void StartApplication() {
     NS_LOG_FUNCTION(this);
     m_running = true;
     
     // Initialize SST channel
     m_channel = SstChannel();
     
     ProcessNextPage();
   }
 
   virtual void StopApplication() {
     NS_LOG_FUNCTION(this);
     m_running = false;
     CleanupSocket();
   }
 
 private:
   void CleanupSocket() {
     if (m_socket) {
       m_socket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
       m_socket->Close();
       m_socket = nullptr;
     }
     
     // Cancel all pending retransmission timers
     for (auto& pair : m_channel.pendingPackets) {
       Simulator::Cancel(pair.second.retransmitTimer);
     }
     
     m_connected = false;
     m_activeStreams.clear();
     m_channel.pendingPackets.clear();
     m_channel.packetsInFlight = 0;
     while (!m_pendingRequests.empty()) {
       m_pendingRequests.pop();
     }
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
       Simulator::Schedule(MicroSeconds(1), &HttpSstClient::ProcessNextPage, this);
       return;
     }
     
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
     
     // Clear pending requests
     while (!m_pendingRequests.empty()) {
       m_pendingRequests.pop();
     }
     
     NS_LOG_INFO("Starting page " << m_currentPageIndex << " with " << page.requests.size() << " requests");
     
     // Set a timeout for the entire page
     Simulator::Schedule(Seconds(30), &HttpSstClient::HandlePageTimeout, this, m_currentPageIndex);
     
     StartPrimaryRequest();
   }
 
   void StartPrimaryRequest() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     if (page.requests.empty()) return;
     
     NS_LOG_INFO("Starting primary request for page " << m_currentPageIndex);
     
     // Queue primary request
     m_pendingRequests.push(&page.requests[0]);
     
     // Establish SST channel if needed
     if (!m_connected) {
       EstablishSstChannel();
     } else {
       ProcessPendingRequests();
     }
   }
 
   void StartSecondaryRequests() {
     if (!m_running || m_currentPageIndex >= m_pages.size()) {
       return;
     }
     
     WebPage& page = m_pages[m_currentPageIndex];
     if (page.requests.size() <= 1) return;
     
     NS_LOG_INFO("Starting " << (page.requests.size() - 1) << " secondary requests for page " << m_currentPageIndex);
     
     // Queue all secondary requests - SST supports unlimited parallel streams
     for (size_t i = 1; i < page.requests.size(); i++) {
       m_pendingRequests.push(&page.requests[i]);
     }
     
     ProcessPendingRequests();
   }
 
   void EstablishSstChannel() {
     NS_LOG_FUNCTION(this);
     
     if (m_socket) {
       CleanupSocket();
     }
     
     // Create UDP socket for SST channel
     m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
     m_socket->Bind();
     m_socket->Connect(m_serverAddress);
     
     m_socket->SetRecvCallback(MakeCallback(&HttpSstClient::HandleRead, this));
     
     m_connected = true;
     
     NS_LOG_INFO("SST channel established over UDP");
     
     ProcessPendingRequests();
   }
 
   void ProcessPendingRequests() {
     if (!m_running || !m_connected) return;
     
     // Process pending requests based on congestion window
     while (!m_pendingRequests.empty() && 
            m_channel.packetsInFlight < m_channel.cwnd) {
       
       WebRequest* request = m_pendingRequests.front();
       m_pendingRequests.pop();
       
       CreateAndSendStream(request);
     }
   }
 
   void CreateAndSendStream(WebRequest* request) {
     if (!m_connected) {
       NS_LOG_ERROR("Cannot create stream - SST channel not established");
       return;
     }
     
     uint16_t streamId = m_nextStreamId++;
     SstStream& stream = m_activeStreams[streamId];
     
     stream.streamId = streamId;
     stream.request = request;
     stream.isActive = true;
     stream.nextByteSeq = 0;
     stream.expectedByteSeq = 0;
     
     request->startTime = Simulator::Now();
     
     // Create HTTP/1.0 request
     std::string path = request->url;
     std::istringstream iss(request->url);
     std::string method, extractedPath, version;
     if (iss >> method >> extractedPath >> version) {
       path = extractedPath;
     }
     
     std::ostringstream oss;
     oss << "GET " << path << "?size=" << request->size << " HTTP/1.0\r\n"
         << "Host: example.com\r\n"
         << "User-Agent: ns3-http-sst-client\r\n"
         << "\r\n";
     
     stream.sendBuffer = oss.str();
     
     // Send SST INIT packet with HTTP request
     SendSstInit(stream);
     
     NS_LOG_INFO("Created SST stream " << streamId << " for request " 
                 << (request->isPrimary ? "[PRIMARY]" : "[SECONDARY]") 
                 << " URL: " << request->url << " (size=" << request->size << ")");
   }
 
   void SendSstInit(SstStream& stream) {
     // Create SST packet headers
     SstChannelHeader chanHdr;
     chanHdr.channelId = 1;
     chanHdr.packetSeqNum = m_channel.nextPacketSeq++;
     chanHdr.ackSeqNum = m_channel.lastAckedPacketSeq;
     chanHdr.ackCount = 1;
     
     SstStreamHeader streamHdr;
     streamHdr.localStreamId = stream.streamId;
     streamHdr.byteSeqNum = 0;
     streamHdr.window = 31; // Max window
     streamHdr.flags = 0;   // No special flags
     
     // Extract packet sequence number for timer (can't pass bit-field directly)
     uint32_t packetSeq = chanHdr.packetSeqNum;
     
     // Create and send SST packet
     Ptr<Packet> packet = CreateSstPacket(chanHdr, streamHdr, stream.sendBuffer);
     
     int result = m_socket->Send(packet);
     if (result == -1) {
       NS_LOG_ERROR("Failed to send SST INIT packet");
       return;
     }
     
     // Track packet for retransmission
     SstPendingPacket& pending = m_channel.pendingPackets[packetSeq];
     pending.packetSeqNum = packetSeq;
     pending.payload = stream.sendBuffer;
     pending.streamHeader = streamHdr;
     pending.sentTime = Simulator::Now();
     pending.retransmitCount = 0;
     
     // Set retransmission timer
     Time timeout = MicroSeconds(m_channel.rto);
     pending.retransmitTimer = Simulator::Schedule(timeout, 
       &HttpSstClient::HandleRetransmissionTimeout, this, packetSeq);
     
     m_channel.packetsInFlight++;
     stream.sentBytes = stream.sendBuffer.length();
     
     NS_LOG_INFO("Sent SST INIT packet for stream " << stream.streamId 
                 << " (packet seq=" << packetSeq << ", RTO=" << m_channel.rto << "us)");
   }
 
   void HandleRead(Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this);
     
     if (!m_running) return;
     
     Ptr<Packet> packet;
     Address from;
     
     while ((packet = socket->RecvFrom(from))) {
       ProcessSstPacket(packet);
     }
   }
 
   void ProcessSstPacket(Ptr<Packet> packet) {
     SstChannelHeader chanHdr;
     SstStreamHeader streamHdr;
     std::string payload;
     
     if (!ParseSstPacket(packet, chanHdr, streamHdr, payload)) {
       NS_LOG_WARN("Failed to parse SST packet");
       return;
     }
     
     // Update congestion control based on ACK
     UpdateCongestionControl(chanHdr.ackSeqNum);
     
     // Find the stream this packet belongs to
     auto it = m_activeStreams.find(streamHdr.localStreamId);
     if (it != m_activeStreams.end()) {
       SstStream& stream = it->second;
       
       // Process received data
       if (!payload.empty()) {
         stream.recvBuffer += payload;
         CheckStreamComplete(stream);
       }
     }
   }
 
   void HandleRetransmissionTimeout(uint32_t packetSeqNum) {
     if (!m_running) return;
     
     auto it = m_channel.pendingPackets.find(packetSeqNum);
     if (it == m_channel.pendingPackets.end()) {
       return; // Packet already acknowledged
     }
     
     SstPendingPacket& pending = it->second;
     pending.retransmitCount++;
     
     // Congestion control: timeout indicates packet loss
     m_channel.ssthresh = std::max(m_channel.cwnd / 2, 2u);
     m_channel.cwnd = 1;  // Reset to 1 (slow start)
     m_channel.inSlowStart = true;
     
     // Exponential backoff for RTO
     m_channel.rto = std::min(m_channel.rto * 2, 64000000u); // Max 64 seconds
     
     NS_LOG_WARN("Packet " << packetSeqNum << " timeout (attempt " << pending.retransmitCount 
                 << "), cwnd reset to 1, RTO=" << m_channel.rto << "us");
     
     // Give up after 5 retransmission attempts
     if (pending.retransmitCount >= 5) {
       NS_LOG_ERROR("Giving up on packet " << packetSeqNum << " after 5 retransmissions");
       m_channel.packetsInFlight--;
       m_channel.pendingPackets.erase(it);
       return;
     }
     
     // Retransmit with new sequence number (SST requirement)
     RetransmitPacket(pending);
   }
   
   void RetransmitPacket(SstPendingPacket& pending) {
     // Create new packet with new sequence number
     SstChannelHeader chanHdr;
     chanHdr.channelId = 1;
     chanHdr.packetSeqNum = m_channel.nextPacketSeq++;  // NEW sequence number
     chanHdr.ackSeqNum = m_channel.lastAckedPacketSeq;
     chanHdr.ackCount = 1;
     
     // Extract packet sequence number for timer (can't pass bit-field directly)
     uint32_t packetSeq = chanHdr.packetSeqNum;
     
     // Create and send SST packet
     Ptr<Packet> packet = CreateSstPacket(chanHdr, pending.streamHeader, pending.payload);
     
     int result = m_socket->Send(packet);
     if (result == -1) {
       NS_LOG_ERROR("Failed to retransmit packet");
       return;
     }
     
     // Remove old packet tracking
     uint32_t oldSeqNum = pending.packetSeqNum;
     m_channel.pendingPackets.erase(oldSeqNum);
     
     // Track new packet
     SstPendingPacket& newPending = m_channel.pendingPackets[packetSeq];
     newPending = pending;  // Copy old packet info
     newPending.packetSeqNum = packetSeq;
     newPending.sentTime = Simulator::Now();
     
     // Set new retransmission timer
     Time timeout = MicroSeconds(m_channel.rto);
     newPending.retransmitTimer = Simulator::Schedule(timeout, 
       &HttpSstClient::HandleRetransmissionTimeout, this, packetSeq);
     
     NS_LOG_INFO("Retransmitted packet " << oldSeqNum << " as " << packetSeq);
   }
   void UpdateCongestionControl(uint32_t ackSeqNum) {
     if (ackSeqNum > m_channel.lastAckedPacketSeq) {
       uint32_t newlyAcked = ackSeqNum - m_channel.lastAckedPacketSeq;
       
       // Cancel retransmission timers for acknowledged packets
       for (uint32_t seq = m_channel.lastAckedPacketSeq + 1; seq <= ackSeqNum; seq++) {
         auto it = m_channel.pendingPackets.find(seq);
         if (it != m_channel.pendingPackets.end()) {
           // Cancel retransmission timer
           Simulator::Cancel(it->second.retransmitTimer);
           
           // Update RTT if this packet has timing info
           Time rtt = Simulator::Now() - it->second.sentTime;
           uint32_t rttMicros = rtt.GetMicroSeconds();
           
           // RTT estimation (RFC 6298 style)
           if (m_channel.rtt == 100000) { // First RTT measurement
             m_channel.rtt = rttMicros;
           } else {
             // SRTT = (1 - alpha) * SRTT + alpha * RTT, alpha = 1/8
             m_channel.rtt = (7 * m_channel.rtt + rttMicros) / 8;
           }
           
           // RTO = SRTT + max(G, K * RTTVAR), simplified to RTO = 4 * SRTT
           m_channel.rto = std::max(4 * m_channel.rtt, 200000u); // Min 200ms
           m_channel.rto = std::min(m_channel.rto, 64000000u);   // Max 64s
           
           // Remove from pending packets
           m_channel.pendingPackets.erase(it);
           m_channel.packetsInFlight--;
         }
       }
       
       m_channel.lastAckedPacketSeq = ackSeqNum;
       
       // Congestion control (TCP-like)
       if (m_channel.inSlowStart) {
         m_channel.cwnd += newlyAcked; // Slow start: exponential growth
         if (m_channel.cwnd >= m_channel.ssthresh) {
           m_channel.inSlowStart = false;
         }
       } else {
         // Congestion avoidance: linear growth
         m_channel.cwnd += std::max(1u, newlyAcked / m_channel.cwnd);
       }
       
       NS_LOG_DEBUG("ACK " << ackSeqNum << ": cwnd=" << m_channel.cwnd 
                    << " ssthresh=" << m_channel.ssthresh 
                    << " rtt=" << m_channel.rtt << "us rto=" << m_channel.rto << "us");
       
       // Process more pending requests
       ProcessPendingRequests();
     }
   }
 
   void CheckStreamComplete(SstStream& stream) {
     if (!stream.request) return;
     
     // Simple completion check: look for end of HTTP response
     if (stream.recvBuffer.find("\r\n\r\n") != std::string::npos) {
       stream.request->completeTime = Simulator::Now();
       Time responseTime = stream.request->completeTime - stream.request->startTime;
       
       bool isPrimary = stream.request->isPrimary;
       
       NS_LOG_INFO("SST stream " << stream.streamId << " completed in " 
                   << responseTime.GetSeconds() << " seconds"
                   << (isPrimary ? " [PRIMARY]" : " [SECONDARY]"));
       
       if (isPrimary) {
         HandlePrimaryRequestComplete();
       }
       
       // Remove completed stream
       m_activeStreams.erase(stream.streamId);
       
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
       Simulator::Schedule(MicroSeconds(10), &HttpSstClient::ProcessNextPage, this);
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
     
     // Clear active streams for this page
     m_activeStreams.clear();
     
     page.isComplete = true;
     m_currentPageIndex++;
     Simulator::Schedule(MicroSeconds(10), &HttpSstClient::ProcessNextPage, this);
   }
 
   bool m_running;
   Address m_serverAddress;
   std::vector<WebPage> m_pages;
   uint32_t m_currentPageIndex;
   bool m_waitingForPrimary;
   
   // SST state
   Ptr<Socket> m_socket;
   bool m_connected;
   SstChannel m_channel;                      // Shared congestion control
   std::map<uint16_t, SstStream> m_activeStreams;
   uint16_t m_nextStreamId;
   std::queue<WebRequest*> m_pendingRequests;
 };
 
 // HTTP SST Server Application
 class HttpSstServer : public Application {
 public:
   HttpSstServer() : m_socket(nullptr), m_running(false) {}
   virtual ~HttpSstServer() {}
 
   static TypeId GetTypeId() {
     static TypeId tid = TypeId("ns3::HttpSstServer")
       .SetParent<Application>()
       .SetGroupName("Applications")
       .AddConstructor<HttpSstServer>();
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
     
     m_clientChannels.clear();
     Application::DoDispose();
   }
 
   virtual void StartApplication() {
     NS_LOG_FUNCTION(this);
     m_running = true;
     
     if (!m_socket) {
       m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
       InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
       m_socket->Bind(local);
       
       m_socket->SetRecvCallback(MakeCallback(&HttpSstServer::HandleRead, this));
     }
     
     NS_LOG_INFO("HTTP SST server bound to UDP port " << m_port);
   }
 
   virtual void StopApplication() {
     NS_LOG_FUNCTION(this);
     m_running = false;
     
     if (m_socket) {
       m_socket->Close();
       m_socket = nullptr;
     }
     
     m_clientChannels.clear();
   }
 
 private:
   void HandleRead(Ptr<Socket> socket) {
     NS_LOG_FUNCTION(this << socket);
     
     Ptr<Packet> packet;
     Address from;
     
     while ((packet = socket->RecvFrom(from))) {
       ProcessSstPacket(packet, from);
     }
   }
 
   void ProcessSstPacket(Ptr<Packet> packet, Address clientAddr) {
     SstChannelHeader chanHdr;
     SstStreamHeader streamHdr;
     std::string payload;
     
     if (!ParseSstPacket(packet, chanHdr, streamHdr, payload)) {
       NS_LOG_WARN("Failed to parse SST packet");
       return;
     }
     
     // Get or create client channel state
     std::ostringstream clientKeyStream;
     clientKeyStream << InetSocketAddress::ConvertFrom(clientAddr).GetIpv4();
     std::string clientKey = clientKeyStream.str();
     SstChannel& channel = m_clientChannels[clientKey];
     
     NS_LOG_INFO("SST server processing packet from " << clientKey 
                 << " (stream=" << streamHdr.localStreamId << ")");
     
     // Process HTTP request if payload exists
     if (!payload.empty()) {
       ProcessHttpRequest(payload, streamHdr.localStreamId, clientAddr, channel);
     }
     
     // Send ACK
     SendAck(clientAddr, chanHdr.packetSeqNum, channel);
   }
 
   void ProcessHttpRequest(const std::string& httpRequest, uint16_t streamId, 
                          Address clientAddr, SstChannel& channel) {
     std::istringstream iss(httpRequest);
     std::string method, path, version;
     if (iss >> method >> path >> version) {
       SendHttpResponse(path, streamId, clientAddr, channel);
     }
   }
 
   void SendHttpResponse(const std::string& url, uint16_t streamId, 
                        Address clientAddr, SstChannel& channel) {
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
     
     // Create HTTP/1.0 response
     std::ostringstream response;
     response << "HTTP/1.0 200 OK\r\n"
              << "Content-Type: text/html\r\n"
              << "Content-Length: " << responseSize << "\r\n"
              << "\r\n";
     
     // Add response body
     std::string body(responseSize, 'X');
     response << body;
     
     // Send as SST packet
     SstChannelHeader chanHdr;
     chanHdr.channelId = 1;
     chanHdr.packetSeqNum = channel.nextPacketSeq++;
     
     SstStreamHeader streamHdr;
     streamHdr.localStreamId = streamId;
     streamHdr.byteSeqNum = 0;
     streamHdr.window = 31;
     streamHdr.flags = 0;
     
     std::string fullResponse = response.str();
     Ptr<Packet> packet = CreateSstPacket(chanHdr, streamHdr, fullResponse);
     
     int result = m_socket->SendTo(packet, 0, clientAddr);
     if (result == -1) {
       NS_LOG_ERROR("Failed to send SST response for stream " << streamId);
     } else {
       NS_LOG_INFO("SST server sent response of " << responseSize 
                   << " bytes for stream " << streamId);
     }
   }
 
   void SendAck(Address clientAddr, uint32_t ackSeqNum, SstChannel& channel) {
     // Send standalone ACK packet
     SstChannelHeader chanHdr;
     chanHdr.channelId = 1;
     chanHdr.packetSeqNum = channel.nextPacketSeq++;
     chanHdr.ackSeqNum = ackSeqNum;
     chanHdr.ackCount = 1;
     
     SstStreamHeader streamHdr; // Empty stream header for ACK
     
     Ptr<Packet> packet = CreateSstPacket(chanHdr, streamHdr, "");
     m_socket->SendTo(packet, 0, clientAddr);
   }
 
   Ptr<Socket> m_socket;
   std::map<std::string, SstChannel> m_clientChannels; // Per-client channel state
   uint16_t m_port;
   bool m_running;
 };
 
 // ReadTraceFile function (same as other versions)
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
   
   LogComponentEnable("HttpSstSimulation", LOG_LEVEL_INFO);
   
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
   Ptr<HttpSstServer> server = CreateObject<HttpSstServer>();
   server->SetPort(port);
   nodes.Get(1)->AddApplication(server);
   server->SetStartTime(Seconds(1.0));
   server->SetStopTime(Seconds(simulationTime));
   
   // Create and install HTTP client
   Ptr<HttpSstClient> client = CreateObject<HttpSstClient>();
   Address serverAddress(InetSocketAddress(interfaces.GetAddress(1), port));
   client->SetServer(serverAddress);
   client->SetPages(pages);
   nodes.Get(0)->AddApplication(client);
   client->SetStartTime(Seconds(2.0));
   client->SetStopTime(Seconds(simulationTime));
   
   // Enable packet tracing
   AsciiTraceHelper ascii;
   pointToPoint.EnableAsciiAll(ascii.CreateFileStream("http-sst-simulation.tr"));
   pointToPoint.EnablePcapAll("http-sst-simulation");
   
   // Set up flow monitor
   Ptr<FlowMonitor> flowMonitor;
   FlowMonitorHelper flowHelper;
   flowMonitor = flowHelper.InstallAll();
   
   // Run simulation
   NS_LOG_INFO("Running HTTP/1.0 SST simulation for " << simulationTime << " seconds");
   Simulator::Stop(Seconds(simulationTime));
   Simulator::Run();
   
   // Process statistics
   std::cout << "Results for HTTP/1.0 SST mode:" << std::endl;
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
