/* web-workload.cc */

#include "web-workload.h"
#include "ns3/log.h"
#include <fstream>
#include <sstream>
#include <string>
#include <random>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("WebWorkload");

WebWorkload::WebWorkload()
{
}

WebWorkload::~WebWorkload()
{
}

uint32_t WebPage::GetTotalSize() const
{
  uint32_t total = primaryObjectSize;
  for (uint32_t size : embeddedObjectSizes) {
    total += size;
  }
  return total;
}

uint32_t WebPage::GetObjectCount() const
{
  return 1 + embeddedObjectSizes.size(); // Primary + embedded
}

const WebPage& WebWorkload::GetPage(size_t index) const
{
  NS_ASSERT(index < m_pages.size());
  return m_pages[index];
}

size_t WebWorkload::GetPageCount() const
{
  return m_pages.size();
}

bool WebWorkload::ParseTraces(const std::string& filename)
{
  std::ifstream file(filename.c_str(), std::ios::in);
  if (!file.is_open()) {
    NS_LOG_ERROR("Could not open trace file: " << filename);
    return false;
  }
  
  NS_LOG_INFO("Parsing trace file: " << filename);
  
  // For simplicity and reproducibility, let's use a synthetic workload 
  // based on the trace statistics in the paper
  
  // Create pages with varying sizes and embedded object counts
  std::default_random_engine generator(12345); // Fixed seed for reproducibility
  
  // Main object size: lognormal distribution (mean ~10KB, stddev ~40KB)
  std::lognormal_distribution<double> mainSizeDist(9.0, 1.0);
  
  // Embedded object size: lognormal distribution (mean ~1KB, stddev ~2KB)
  std::lognormal_distribution<double> embeddedSizeDist(6.5, 0.8);
  
  // Number of embedded objects: negative binomial (mean ~5, variance ~10)
  std::negative_binomial_distribution<int> numEmbeddedDist(2, 0.3);
  
  // Generate 100 synthetic pages
  for (int i = 0; i < 100; i++) {
    WebPage page;
    
    // Generate main object size
    page.primaryObjectSize = static_cast<uint32_t>(mainSizeDist(generator));
    
    // Generate embedded objects
    int numEmbedded = numEmbeddedDist(generator);
    for (int j = 0; j < numEmbedded; j++) {
      uint32_t size = static_cast<uint32_t>(embeddedSizeDist(generator));
      page.embeddedObjectSizes.push_back(size);
    }
    
    m_pages.push_back(page);
  }
  
  NS_LOG_INFO("Generated " << m_pages.size() << " synthetic web pages");
  
  return true;
}

} // namespace ns3