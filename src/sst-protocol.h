/* sst-protocol.h */

#ifndef SST_PROTOCOL_H
#define SST_PROTOCOL_H

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include <map>
#include <vector>

namespace ns3 {

class SstStream;
class SstChannel;

// SST Channel class to manage packet sequencing, ACKs, and congestion control
class SstChannel : public Object
{
public:
  static TypeId GetTypeId(void);
  SstChannel();
  virtual ~SstChannel();
  
  // Initialize the channel with remote endpoint
  bool Initialize(Ipv4Address remoteAddress, uint16_t remotePort);
  
  // Create a new stream on this channel
  Ptr<SstStream> CreateStream();
  
  // Get congestion window size
  uint32_t GetCwnd() const;
  
  // Send data on behalf of a stream
  bool SendData(uint16_t streamId, Ptr<Packet> packet);
  
  // Process incoming data
  void ProcessIncomingData(Ptr<Packet> packet);
  
private:
  Ipv4Address m_remoteAddress;
  uint16_t m_remotePort;
  uint32_t m_cwnd; // Congestion window
  uint16_t m_nextStreamId;
  Ptr<Socket> m_socket;
  
  std::map<uint16_t, Ptr<SstStream>> m_streams;
  
  void SocketRecvCallback(Ptr<Socket> socket);
};

// SST Stream class for reliable data transfer
class SstStream : public Object
{
public:
  static TypeId GetTypeId(void);
  SstStream(Ptr<SstChannel> channel, uint16_t streamId);
  virtual ~SstStream();
  
  // Send data on the stream
  bool Send(Ptr<Packet> packet);
  
  // Create a child stream
  Ptr<SstStream> CreateSubstream();
  
  // Close the stream
  void Close();
  
  // Set callbacks
  typedef Callback<void, Ptr<SstStream>, Ptr<Packet>> RecvCallback;
  void SetRecvCallback(RecvCallback callback);
  
  // Process received data
  void ProcessData(Ptr<Packet> packet);
  
  // Get stream ID
  uint16_t GetStreamId() const;
  
private:
  Ptr<SstChannel> m_channel;
  uint16_t m_streamId;
  bool m_isOpen;
  Ptr<SstStream> m_parent;
  std::vector<Ptr<SstStream>> m_children;
  RecvCallback m_recvCallback;
};

// SST Socket class for application interface
class SstSocket : public Socket
{
public:
  static TypeId GetTypeId(void);
  SstSocket();
  virtual ~SstSocket();
  
  // Create a new substream
  Ptr<SstStream> CreateSubstream();
  
  // Override Socket methods
  virtual enum SocketErrno GetErrno() const;
  virtual enum SocketType GetSocketType() const;
  virtual Ptr<Node> GetNode() const;
  virtual int Bind();
  virtual int Bind6();
  virtual int Bind(const Address& address);
  virtual int Connect(const Address& address);
  virtual int Listen();
  virtual int Close();
  virtual int ShutdownSend();
  virtual int ShutdownRecv();
  virtual int Send(Ptr<Packet> p, uint32_t flags);
  virtual int SendTo(Ptr<Packet> p, uint32_t flags, const Address& toAddress);
  virtual Ptr<Packet> Recv(uint32_t maxSize, uint32_t flags);
  virtual Ptr<Packet> RecvFrom(uint32_t maxSize, uint32_t flags, Address& fromAddress);
  virtual uint32_t GetTxAvailable() const;
  virtual uint32_t GetRxAvailable() const;
  virtual int GetSockName(Address& address) const;
  virtual int GetPeerName(Address& address) const;
  virtual bool SetAllowBroadcast(bool allowBroadcast);
  virtual bool GetAllowBroadcast() const;
  
private:
  Ptr<SstChannel> m_channel;
  Ptr<SstStream> m_rootStream;
  Ptr<Node> m_node;
  Address m_peerAddress;
  mutable enum SocketErrno m_errno;
};

} // namespace ns3

#endif /* SST_PROTOCOL_H */