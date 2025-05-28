#!/usr/bin/env python3
"""
UCB Binary Trace Parser - Corrected for SST Paper Methodology

This script implements the exact methodology described in Section 5.5 of the SST paper:
1. Sort by client IP address
2. Group requests by primary/secondary pattern (ignore timestamps)
3. Associate contiguous secondary requests with preceding primary request
"""

import sys
import struct
import os

# Defining the binary record structure as per logparse.h
HEADER_SIZE = 60  # 60 bytes for the fixed header

def parse_binary_trace(trace_file, output_file):
    """Parse the binary UCB trace file using SST paper methodology"""
    
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
                
                try:
                    # Unpack the binary header data (big-endian format)
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
                    
                    # Convert URL bytes to string
                    url = url_data.decode('utf-8', errors='replace')
                    
                    # Create request record with client IP for sorting
                    request = {
                        'id': record_id,
                        'url': url,
                        'client_ip': client_ip,  # Important: include client IP
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
                    break
        
        print(f"Successfully parsed {len(requests)} records from {trace_file}")
        
        # STEP 1: Sort by client IP address (as specified in paper)
        print("Sorting requests by client IP address...")
        requests.sort(key=lambda x: x['client_ip'])
        
        # STEP 2: Group requests into pages using SST paper methodology
        pages = group_requests_into_pages_sst_method(requests)
        
        # Write to output file
        write_pages_to_file(pages, output_file)
        
    except Exception as e:
        print(f"Error processing trace file: {e}")
        return False
    
    return True

def is_primary_request(url):
    """Determine if URL is likely a primary (HTML) request"""
    url_lower = url.lower()
    
    # Check for common secondary resource suffixes
    secondary_extensions = ['.gif', '.jpg', '.jpeg', '.png', '.css', '.js', '.ico', '.bmp']
    for ext in secondary_extensions:
        if ext in url_lower:
            return False
    
    # Check for CGI flag (but not CSS files)
    if '.c' in url and '.css' not in url_lower:
        return False
    
    # Primary requests: .html, .htm, or no obvious extension
    primary_extensions = ['.html', '.htm']
    for ext in primary_extensions:
        if ext in url_lower:
            return True
    
    # If no clear extension, likely primary (like directory requests)
    if '.' not in url.split('/')[-1]:  # No extension in last part of URL
        return True
    
    return True  # Default to primary if uncertain

def group_requests_into_pages_sst_method(requests):
    """
    Group requests into pages using exact SST paper methodology:
    'associating each contiguous run of secondary requests with the 
    immediately preceding primary request'
    """
    pages = []
    current_page = []
    current_client_ip = None
    
    for req in requests:
        # If we switched to a new client, start fresh
        if current_client_ip != req['client_ip']:
            # Save current page if it exists
            if current_page:
                pages.append(current_page)
                current_page = []
            current_client_ip = req['client_ip']
        
        # If this is a primary request
        if req['is_primary']:
            # Save the previous page if it exists
            if current_page:
                pages.append(current_page)
                current_page = []
            
            # Start new page with this primary request
            current_page = [req]
        
        # If this is a secondary request
        else:
            # Add to current page if one exists
            if current_page:
                current_page.append(req)
            else:
                # Orphaned secondary request - create a page for it
                # (This handles edge cases where secondary requests come first)
                current_page = [req]
    
    # Add the last page if not empty
    if current_page:
        pages.append(current_page)
    
    # Filter out pages that are too small or have issues
    valid_pages = []
    for page in pages:
        if len(page) > 0:
            # Ensure each page has at least one primary request
            has_primary = any(req['is_primary'] for req in page)
            if not has_primary and len(page) > 0:
                # Convert first request to primary if no primary exists
                page[0]['is_primary'] = True
            valid_pages.append(page)
    
    print(f"Grouped {len(requests)} requests from {len(set(req['client_ip'] for req in requests))} clients into {len(valid_pages)} pages")
    return valid_pages

def write_pages_to_file(pages, output_file):
    """Write pages to output file in format suitable for NS-3 simulation"""
    with open(output_file, 'w') as f:
        f.write("# URL,SIZE_IN_BYTES,IS_PRIMARY(1=true,0=false),REQUEST_TIME,RESPONSE_TIME\n")
        
        for i, page in enumerate(pages):
            f.write(f"# --- Page {i+1} ({len(page)} requests) ---\n")
            
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