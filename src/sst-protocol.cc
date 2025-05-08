/* sst-protocol.cc */

#include "sst-protocol.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/inet-socket-address.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("SstProtocol");

/***************************
 * SstChannel Implementation
 ***************************/

NS_OBJECT_ENSURE_REGISTERED(SstChannel);

TypeId SstChannel::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::SstChannel")
    .SetParent<Object>()
    .AddConstructor<SstChannel>();
  return tid;
}

SstChannel::SstChannel()
  : m_cwnd(10), m_nextStreamId(1), m_socket(0)
{
  NS_LOG_FUNCTION(this);
}

SstChannel::~SstChannel()
{
  NS_LOG_FUNCTION(this);
  m_socket = 0;
}

bool SstChannel::Initialize(Ipv4Address remoteAddress, uint16_t remotePort)
{
  NS_LOG_FUNCTION(this << remoteAddress << remotePort);
  m_remoteAddress = remoteAddress;
  m_remotePort = remotePort;
  
  // Create underlying TCP socket
  TypeId tid = TypeId::LookupByName("ns3::TcpSocketFactory");
  m_socket = Socket::CreateSocket(GetObject<Node>(), tid);
  
  m_socket->Bind();
  m_socket->SetRecvCallback(MakeCallback(&SstChannel::SocketRecvCallback, this));
  
  return m_socket->Connect(InetSocketAddress(remoteAddress, remotePort)) == 0;
}

Ptr<SstStream> SstChannel::CreateStream()
{
  NS_LOG_FUNCTION(this);
  uint16_t streamId = m_nextStreamId++;
  
  Ptr<SstStream> stream = CreateObject<SstStream>(this, streamId);
  m_streams[streamId] = stream;
  
  return stream;
}

uint32_t SstChannel::GetCwnd() const
{
  return m_cwnd;
}

bool SstChannel::SendData(uint16_t streamId, Ptr<Packet> packet)
{
  NS_LOG_FUNCTION(this << streamId << packet);
  
  // Add stream ID header
  struct StreamHeader
  {
    uint16_t streamId;
  } header;
  
  header.streamId = streamId;
  packet->AddHeader(header);
  
  // Send through underlying socket
  return m_socket->Send(packet) >= 0;
}

void SstChannel::SocketRecvCallback(Ptr<Socket> socket)
{
  NS_LOG_FUNCTION(this << socket);
  
  Ptr<Packet> packet;
  while ((packet = socket->Recv()))
  {
    ProcessIncomingData(packet);
  }
}

void SstChannel::ProcessIncomingData(Ptr<Packet> packet)
{
  NS_LOG_FUNCTION(this << packet);
  
  // Extract stream ID from header
  struct StreamHeader
  {
    uint16_t streamId;
  } header;
  
  packet->RemoveHeader(header);
  
  // Find the target stream
  auto it = m_streams.find(header.streamId);
  if (it != m_streams.end())
  {
    it->second->ProcessData(packet);
  }
  else
  {
    NS_LOG_WARN("Received data for unknown stream ID: " << header.streamId);
  }
}

/***************************
 * SstStream Implementation
 ***************************/

NS_OBJECT_ENSURE_REGISTERED(SstStream);

TypeId SstStream::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::SstStream")
    .SetParent<Object>()
    .AddConstructor<SstStream>();
  return tid;
}

SstStream::SstStream(Ptr<SstChannel> channel, uint16_t streamId)
  : m_channel(channel), m_streamId(streamId), m_isOpen(true), m_parent(0)
{
  NS_LOG_FUNCTION(this << channel << streamId);
}

SstStream::~SstStream()
{
  NS_LOG_FUNCTION(this);
}

bool SstStream::Send(Ptr<Packet> packet)
{
  NS_LOG_FUNCTION(this << packet);
  
  if (!m_isOpen)
  {
    NS_LOG_WARN("Attempt to send on closed stream");
    return false;
  }
  
  return m_channel->SendData(m_streamId, packet);
}

Ptr<SstStream> SstStream::CreateSubstream()
{
  NS_LOG_FUNCTION(this);
  
  Ptr<SstStream> child = m_channel->CreateStream();
  child->m_parent = this;
  m_children.push_back(child);
  
  return child;
}

void SstStream::Close()
{
  NS_LOG_FUNCTION(this);
  m_isOpen = false;
  
  // Close all child streams
  for (auto child : m_children)
  {
    child->Close();
  }
}

void SstStream::SetRecvCallback(RecvCallback callback)
{
  NS_LOG_FUNCTION(this);
  m_recvCallback = callback;
}

void SstStream::ProcessData(Ptr<Packet> packet)
{
  NS_LOG_FUNCTION(this << packet);
  
  if (!m_recvCallback.IsNull())
  {
    m_recvCallback(this, packet);
  }
}

uint16_t SstStream::GetStreamId() const
{
  return m_streamId;
}

/***************************
 * SstSocket Implementation
 ***************************/

NS_OBJECT_ENSURE_REGISTERED(SstSocket);

TypeId SstSocket::GetTypeId(void)
{
  static TypeId tid = TypeId("ns3::SstSocket")
    .SetParent<Socket>()
    .AddConstructor<SstSocket>();
  return tid;
}

SstSocket::SstSocket()
  : m_channel(0), m_rootStream(0), m_node(0), m_errno(ERROR_NOTERROR)
{
  NS_LOG_FUNCTION(this);
  m_channel = CreateObject<SstChannel>();
}

SstSocket::~SstSocket()
{
  NS_LOG_FUNCTION(this);
}

Ptr<SstStream> SstSocket::CreateSubstream()
{
  NS_LOG_FUNCTION(this);
  
  if (!m_rootStream)
  {
    NS_LOG_WARN("Cannot create substream before socket is connected");
    return 0;
  }
  
  return m_rootStream->CreateSubstream();
}

enum Socket::SocketErrno SstSocket::GetErrno() const
{
  return m_errno;
}

enum Socket::SocketType SstSocket::GetSocketType() const
{
  return NS3_SOCK_STREAM;
}

Ptr<Node> SstSocket::GetNode() const
{
  return m_node;
}

int SstSocket::Bind()
{
  NS_LOG_FUNCTION(this);
  m_errno = ERROR_OPNOTSUPP;
  return -1;
}

int SstSocket::Bind6()
{
  NS_LOG_FUNCTION(this);
  m_errno = ERROR_OPNOTSUPP;
  return -1;
}

int SstSocket::Bind(const Address& address)
{
  NS_LOG_FUNCTION(this << address);
  m_errno = ERROR_OPNOTSUPP;
  return -1;
}

int SstSocket::Connect(const Address& address)
{
  NS_LOG_FUNCTION(this << address);
  
  if (!m_node)
  {
    m_node = GetObject<Node>();
  }
  
  if (InetSocketAddress::IsMatchingType(address))
  {
    InetSocketAddress inetAddr = InetSocketAddress::ConvertFrom(address);
    m_peerAddress = address;
    
    if (m_channel->Initialize(inetAddr.GetIpv4(), inetAddr.GetPort()))
    {
      m_rootStream = m_channel->CreateStream();
      return 0;
    }
  }
  
  m_errno = ERROR_INVAL;
  return -1;
}

int SstSocket::Listen()
{
  NS_LOG_FUNCTION(this);
  m_errno = ERROR_OPNOTSUPP;
  return -1;
}

int SstSocket::Close()
{
  NS_LOG_FUNCTION(this);
  
  if (m_rootStream)
  {
    m_rootStream->Close();
    m_rootStream = 0;
  }
  
  return 0;
}

int SstSocket::ShutdownSend()
{
  NS_LOG_FUNCTION(this);
  m_errno = ERROR_OPNOTSUPP;
  return -1;
}

int SstSocket::ShutdownRecv()
{
  NS_LOG_FUNCTION(this);
  m_errno = ERROR_OPNOTSUPP;
  return -1;
}

int SstSocket::Send(Ptr<Packet> p, uint32_t flags)
{
  NS_LOG_FUNCTION(this << p << flags);
  
  if (m_rootStream)
  {
    if (m_rootStream->Send(p))
    {
      return p->GetSize();
    }
  }
  
  m_errno = ERROR_NOTCONN;
  return -1;
}

int SstSocket::SendTo(Ptr<Packet> p, uint32_t flags, const Address& toAddress)
{
  NS_LOG_FUNCTION(this << p << flags << toAddress);
  m_errno = ERROR_OPNOTSUPP;
  return -1;
}

Ptr<Packet> SstSocket::Recv(uint32_t maxSize, uint32_t flags)
{
  NS_LOG_FUNCTION(this << maxSize << flags);
  m_errno = ERROR_OPNOTSUPP;
  return 0;
}

Ptr<Packet> SstSocket::RecvFrom(uint32_t maxSize, uint32_t flags, Address& fromAddress)
{
  NS_LOG_FUNCTION(this << maxSize << flags);
  m_errno = ERROR_OPNOTSUPP;
  return 0;
}

uint32_t SstSocket::GetTxAvailable() const
{
  NS_LOG_FUNCTION(this);
  return 0;
}

uint32_t SstSocket::GetRxAvailable() const
{
  NS_LOG_FUNCTION(this);
  return 0;
}

int SstSocket::GetSockName(Address& address) const
{
  NS_LOG_FUNCTION(this);
  m_errno = ERROR_OPNOTSUPP;
  return -1;
}

int SstSocket::GetPeerName(Address& address) const
{
  NS_LOG_FUNCTION(this);
  address = m_peerAddress;
  return 0;
}

bool SstSocket::SetAllowBroadcast(bool allowBroadcast)
{
  NS_LOG_FUNCTION(this << allowBroadcast);
  return false;
}

bool SstSocket::GetAllowBroadcast() const
{
  NS_LOG_FUNCTION(this);
  return false;
}

} // namespace ns3