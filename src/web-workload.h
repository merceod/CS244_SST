/* web-workload.h */

#ifndef WEB_WORKLOAD_H
#define WEB_WORKLOAD_H

#include "ns3/core-module.h"
#include <vector>
#include <string>

namespace ns3 {

// Structure to represent a web page with primary and embedded objects
class WebPage {
public:
  uint32_t primaryObjectSize;
  std::vector<uint32_t> embeddedObjectSizes;
  
  uint32_t GetTotalSize() const;
  uint32_t GetObjectCount() const;
};

// Class to parse and store web workload from UC Berkeley traces
class WebWorkload {
public:
  WebWorkload();
  virtual ~WebWorkload();
  
  // Parse traces from file
  bool ParseTraces(const std::string& filename);
  
  // Get a page by index
  const WebPage& GetPage(size_t index) const;
  
  // Get page count
  size_t GetPageCount() const;
  
private:
  std::vector<WebPage> m_pages;
};

} // namespace ns3

#endif /* WEB_WORKLOAD_H */