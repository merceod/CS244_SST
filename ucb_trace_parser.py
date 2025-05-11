#!/usr/bin/env python3
"""
UCB Binary Trace Parser

This script directly parses the UCB binary trace format based on the
structure defined in logparse.h.
"""

import sys
import struct
import os

# Defining the binary record structure as per logparse.h
# Each record has a fixed-length header followed by a variable-length URL
HEADER_SIZE = 60  # 60 bytes for the fixed header

def parse_binary_trace(trace_file, output_file):
    """Parse the binary UCB trace file directly"""
    
    requests = []
    
    try:
        # Open the trace file in binary mode
        with open(trace_file, 'rb') as f:
            record_id = 0
            
            # Keep reading records until we reach EOF
            while True:
                # Read the 60-byte header
                header_data = f.read(HEADER_SIZE)
                if not header_data or len(header_data) < HEADER_SIZE:
                    break  # End of file
                
                # Unpack the binary header data
                # Format based on lf_entry struct in logparse.h
                try:
                    # The format is network byte order (big-endian)
                    client_req_sec = struct.unpack('>I', header_data[0:4])[0]
                    client_req_usec = struct.unpack('>I', header_data[4:8])[0]
                    server_first_sec = struct.unpack('>I', header_data[8:12])[0]
                    server_first_usec = struct.unpack('>I', header_data[12:16])[0]
                    server_last_sec = struct.unpack('>I', header_data[16:20])[0]
                    server_last_usec = struct.unpack('>I', header_data[20:24])[0]
                    client_ip = struct.unpack('>I', header_data[24:28])[0]
                    client_port = struct.unpack('>H', header_data[28:30])[0]
                    server_ip = struct.unpack('>I', header_data[30:34])[0]
                    server_port = struct.unpack('>H', header_data[34:36])[0]
                    client_pragma = header_data[36]
                    server_pragma = header_data[37]
                    client_if_mod_since = struct.unpack('>I', header_data[38:42])[0]
                    server_expires = struct.unpack('>I', header_data[42:46])[0]
                    server_last_modified = struct.unpack('>I', header_data[46:50])[0]
                    resp_header_len = struct.unpack('>I', header_data[50:54])[0]
                    resp_data_len = struct.unpack('>I', header_data[54:58])[0]
                    url_len = struct.unpack('>H', header_data[58:60])[0]
                    
                    # Read the URL (variable length based on url_len)
                    url_data = f.read(url_len)
                    if len(url_data) < url_len:
                        print(f"Warning: Unexpected EOF reading URL at record {record_id}")
                        break
                    
                    # Convert URL bytes to string, assuming ASCII/UTF-8
                    url = url_data.decode('utf-8', errors='replace')
                    
                    # Create request record
                    request = {
                        'id': record_id,
                        'url': url,
                        'client_req_time': client_req_sec,
                        'server_first_time': server_first_sec,
                        'server_last_time': server_last_sec,
                        'resp_header_len': resp_header_len,
                        'resp_data_len': resp_data_len,
                        'size': resp_header_len + resp_data_len,
                        'is_primary': is_primary_request(url),
                        'response_time': max(1, server_last_sec - server_first_sec)
                    }
                    
                    requests.append(request)
                    record_id += 1
                    
                    # Print progress periodically
                    if record_id % 1000 == 0:
                        print(f"Processed {record_id} records...")
                
                except Exception as e:
                    print(f"Error parsing record {record_id}: {e}")
                    # Try to continue with the next record
                    # This requires finding the start of the next record, which can be tricky
                    # For simplicity, we'll just skip ahead by the URL length
                    try:
                        f.read(url_len)
                    except:
                        # If that fails, we're probably at EOF
                        break
        
        print(f"Successfully parsed {len(requests)} records from {trace_file}")
        
        # Group requests into pages
        pages = group_requests_into_pages(requests)
        
        # Write to output file
        write_pages_to_file(pages, output_file)
        
    except Exception as e:
        print(f"Error processing trace file: {e}")
        return False
    
    return True

def is_primary_request(url):
    """Determine if URL is likely a primary (HTML) request"""
    # Check for common secondary resource suffixes
    if any(ext in url for ext in ['.gif', '.jpg', '.jpeg', '.png', '.css', '.js']):
        return False
    # Check for CGI flag
    if '.c' in url and '.css' not in url:
        return False
    return True

def group_requests_into_pages(requests):
    """Group requests into pages based on timing and request type"""
    pages = []
    current_page = []
    last_primary_time = 0
    PAGE_TIMEOUT = 10  # 10 seconds
    
    for req in requests:
        # Start a new page if:
        # 1. This is a primary request, or
        # 2. There's a significant time gap, or
        # 3. The current page is empty
        if (req['is_primary'] or 
            not current_page or 
            (req['client_req_time'] - last_primary_time > PAGE_TIMEOUT)):
            
            # Save the current page if it's not empty
            if current_page:
                pages.append(current_page)
                current_page = []
            
            # Start new page with this request
            current_page.append(req)
            
            if req['is_primary']:
                last_primary_time = req['client_req_time']
        else:
            # Add this request to current page
            current_page.append(req)
    
    # Add the last page if not empty
    if current_page:
        pages.append(current_page)
    
    print(f"Grouped {len(requests)} requests into {len(pages)} pages")
    return pages

def write_pages_to_file(pages, output_file):
    """Write pages to output file in format suitable for NS-3 simulation"""
    with open(output_file, 'w') as f:
        f.write("# URL,SIZE_IN_BYTES,IS_PRIMARY(1=true,0=false),REQUEST_TIME,RESPONSE_TIME\n")
        
        for i, page in enumerate(pages):
            f.write(f"# --- Page {i+1} ---\n")
            
            for req in page:
                f.write(f"{req['url']},{req['size']},{1 if req['is_primary'] else 0},"
                       f"{req['client_req_time']},{req['response_time']}\n")
            
            f.write(f"# --- End of Page {i+1} ---\n")
    
    print(f"Successfully wrote {len(pages)} pages to {output_file}")

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <trace_file> <output_file.txt>")
        sys.exit(1)
    
    trace_file = sys.argv[1]
    output_file = sys.argv[2]
    
    # Check if the trace file exists
    if not os.path.isfile(trace_file):
        print(f"Error: Trace file '{trace_file}' does not exist")
        sys.exit(1)
    
    # Check if the file is empty
    if os.path.getsize(trace_file) == 0:
        print(f"Error: Trace file '{trace_file}' is empty")
        sys.exit(1)
    
    # Parse the trace file
    success = parse_binary_trace(trace_file, output_file)
    
    if not success:
        print("Failed to process the trace file")
        sys.exit(1)
    
    print("Trace processing completed successfully")

if __name__ == "__main__":
    main()