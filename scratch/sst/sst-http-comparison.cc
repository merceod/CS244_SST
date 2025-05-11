/* sst-http-comparison.cc */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/packet-sink.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/uinteger.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/log.h"

#include "web-workload.h"
#include "http-clients.h"
#include "sst-protocol.h"

#include <iostream>
#include <fstream>
#include <map>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SstHttpComparison");

// Enum to represent different HTTP variants
enum HttpVariant {
  HTTP10_SERIAL,
  HTTP10_PARALLEL,
  HTTP11_PERSISTENT,
  HTTP11_PIPELINED,
  SST_HTTP10
};

// Structure to store result data
struct ResultData {
  uint32_t requestCount;
  uint32_t totalSize;
  Time loadTime;
};

// Global variables to store metrics
std::vector<ResultData> g_results;

// Function to create client application
ApplicationContainer CreateHttpClient(HttpVariant variant, Ptr<Node> clientNode, 
                                     Ipv4Address serverAddress, uint16_t serverPort,
                                     const WebWorkload& workload);

// Function to run simulation for a specific HTTP variant
void RunSimulation(HttpVariant variant, const WebWorkload& workload);

// Function to output results in a format for plotting
void OutputResults(const std::string& filename);

// Main program
int main_old(int argc, char* argv[])
{
  // Command line parameters
  std::string traceFile = "UCB-home-IP-848278026-848292426.tr";
  std::string variantStr = "http10-serial";
  std::string outputDir = "./";
  bool runAll = false;
  
  CommandLine cmd(__FILE__);
  cmd.AddValue("trace", "Trace file to use", traceFile);
  cmd.AddValue("variant", "HTTP variant to simulate (http10-serial, http10-parallel, http11-persistent, http11-pipelined, sst-http10)", variantStr);
  cmd.AddValue("output", "Output directory", outputDir);
  cmd.AddValue("all", "Run all variants", runAll);
  cmd.Parse(argc, argv);
  
  // Set up logging
  LogComponentEnable("SstHttpComparison", LOG_LEVEL_INFO);
  
  // Parse the web workload from traces
  WebWorkload workload;
  if (!workload.ParseTraces(traceFile)) {
    NS_LOG_ERROR("Failed to parse trace file: " << traceFile);
    return 1;
  }
  
  if (runAll) {
    // Run simulation for all variants
    g_results.clear();
    RunSimulation(HTTP10_SERIAL, workload);
    OutputResults(outputDir + "/http10-serial.dat");
    
    g_results.clear();
    RunSimulation(HTTP10_PARALLEL, workload);
    OutputResults(outputDir + "/http10-parallel.dat");
    
    g_results.clear();
    RunSimulation(HTTP11_PERSISTENT, workload);
    OutputResults(outputDir + "/http11-persistent.dat");
    
    g_results.clear();
    RunSimulation(HTTP11_PIPELINED, workload);
    OutputResults(outputDir + "/http11-pipelined.dat");
    
    g_results.clear();
    RunSimulation(SST_HTTP10, workload);
    OutputResults(outputDir + "/sst-http10.dat");
  } else {
    // Run simulation for the specified variant
    HttpVariant variant;
    if (variantStr == "http10-serial") {
      variant = HTTP10_SERIAL;
    } else if (variantStr == "http10-parallel") {
      variant = HTTP10_PARALLEL;
    } else if (variantStr == "http11-persistent") {
      variant = HTTP11_PERSISTENT;
    } else if (variantStr == "http11-pipelined") {
      variant = HTTP11_PIPELINED;
    } else if (variantStr == "sst-http10") {
      variant = SST_HTTP10;
    } else {
      NS_LOG_ERROR("Unknown HTTP variant: " << variantStr);
      return 1;
    }
    
    g_results.clear();
    RunSimulation(variant, workload);
    OutputResults(outputDir + "/" + variantStr + ".dat");
  }
  
  return 0;
}

// Factory function to create the appropriate client based on variant
ApplicationContainer CreateHttpClient(HttpVariant variant, Ptr<Node> clientNode, 
                                     Ipv4Address serverAddress, uint16_t serverPort,
                                     const WebWorkload& workload)
{
  Address serverAddr = InetSocketAddress(serverAddress, serverPort);
  ApplicationContainer clientApps;
  
  Ptr<HttpClientBase> client = nullptr;
  
  switch (variant) {
    case HTTP10_SERIAL:
      client = CreateObject<Http10SerialClient>();
      break;
    case HTTP10_PARALLEL:
      client = CreateObject<Http10ParallelClient>();
      break;
    case HTTP11_PERSISTENT:
      client = CreateObject<Http11PersistentClient>();
      break;
    case HTTP11_PIPELINED:
      client = CreateObject<Http11PipelinedClient>();
      break;
    case SST_HTTP10:
      client = CreateObject<SstHttpClient>();
      break;
  }
  
  client->SetWorkload(&workload);
  client->SetServerAddress(serverAddr);
  clientNode->AddApplication(client);
  clientApps.Add(client);
  
  return clientApps;
}

// Callback to record page load metrics
void RecordPageLoadMetrics(uint32_t requestCount, uint32_t totalSize, Time loadTime)
{
  ResultData result;
  result.requestCount = requestCount;
  result.totalSize = totalSize;
  result.loadTime = loadTime;
  
  g_results.push_back(result);
  
  NS_LOG_INFO("Page loaded: " << requestCount << " objects, " 
              << totalSize << " bytes, " << loadTime.GetMilliSeconds() << " ms");
}

// Function to run simulation for a specific HTTP variant
void RunSimulation(HttpVariant variant, const WebWorkload& workload)
{
  NS_LOG_INFO("Running simulation for variant: " << variant);
  
  // Create nodes
  NodeContainer nodes;
  nodes.Create(2); // One client, one server
  
  // Create channel between client and server
  PointToPointHelper pointToPoint;
  pointToPoint.SetDeviceAttribute("DataRate", StringValue("1.5Mbps")); // As per paper (DSL)
  pointToPoint.SetChannelAttribute("Delay", StringValue("2ms")); // As per paper
  
  NetDeviceContainer devices = pointToPoint.Install(nodes);
  
  // Install internet stack
  InternetStackHelper internet;
  internet.Install(nodes);
  
  // Assign IP addresses
  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer interfaces = ipv4.Assign(devices);
  
  // Set TCP parameters
  Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1460));
  
  // Server is a packet sink that accepts connections
  uint16_t serverPort = 80;
  PacketSinkHelper serverSink("ns3::TcpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), serverPort));
  ApplicationContainer serverApps = serverSink.Install(nodes.Get(1));
  serverApps.Start(Seconds(0.0));
  serverApps.Stop(Seconds(1000.0));
  
  // Create the client application
  ApplicationContainer clientApps = CreateHttpClient(variant, nodes.Get(0), 
                                                    interfaces.GetAddress(1), serverPort, workload);
  clientApps.Start(Seconds(1.0));
  clientApps.Stop(Seconds(500.0));
  
  // Enable traces
  pointToPoint.EnablePcapAll("sst-http-comparison");
  
  // Set up callbacks to record metrics - in a real implementation we'd hook into the client
  // Here we just show the concept
  
  // Run simulation
  Simulator::Run();
  Simulator::Destroy();
  
  NS_LOG_INFO("Simulation complete for variant: " << variant);
}

// Function to output results in a format for plotting
void OutputResults(const std::string& filename)
{
  std::ofstream outFile(filename.c_str());
  if (!outFile.is_open()) {
    NS_LOG_ERROR("Could not open output file: " << filename);
    return;
  }
  
  outFile << "# requestCount totalSize loadTimeMs" << std::endl;
  
  for (const auto& result : g_results) {
    outFile << result.requestCount << " " 
            << result.totalSize << " " 
            << result.loadTime.GetMilliSeconds() << std::endl;
  }
  
  outFile.close();
  NS_LOG_INFO("Results written to: " << filename);
}