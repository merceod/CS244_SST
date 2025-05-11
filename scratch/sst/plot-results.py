#!/usr/bin/env python3

import matplotlib.pyplot as plt
import numpy as np
import sys
import os

# Load data files
def load_data(filename):
    data = np.loadtxt(filename, comments='#')
    if data.size > 0:
        if len(data.shape) == 1:  # Only one row
            data = data.reshape(1, -1)
        return data
    return np.array([])

# Main function
def main():
    # Check arguments
    if len(sys.argv) < 2:
        print("Usage: {} <output_dir>".format(sys.argv[0]))
        sys.exit(1)
    
    output_dir = sys.argv[1]
    
    # Load data for each variant
    variants = [
        ('http10-serial', 'TCP: HTTP/1.0 serial'),
        ('http10-parallel', 'TCP: HTTP/1.0 parallel'),
        ('http11-persistent', 'TCP: HTTP/1.1 persistent'),
        ('http11-pipelined', 'TCP: HTTP/1.1 pipelined'),
        ('sst-http10', 'SST: HTTP/1.0 parallel')
    ]
    
    data_by_variant = {}
    
    for variant_file, variant_label in variants:
        filename = os.path.join(output_dir, variant_file + '.dat')
        if os.path.exists(filename):
            data = load_data(filename)
            if data.size > 0:
                data_by_variant[variant_label] = data
    
    # Create figure similar to Figure 8 in paper
    plt.figure(figsize=(12, 8))
    
    # Group data by request count
    request_count_groups = [1, 2, 4, 8, 16]
    
    for i, request_count in enumerate(request_count_groups):
        plt.subplot(1, 5, i+1)
        
        for variant_label, data in data_by_variant.items():
            # Filter data for this request count group
            if request_count == 1:
                mask = data[:, 0] == 1
            elif request_count == 2:
                mask = data[:, 0] == 2
            elif request_count == 4:
                mask = (data[:, 0] > 2) & (data[:, 0] <= 4)
            elif request_count == 8:
                mask = (data[:, 0] > 4) & (data[:, 0] <= 8)
            else:
                mask = data[:, 0] > 8
            
            if np.any(mask):
                filtered_data = data[mask]
                # Plot size vs. time
                plt.scatter(filtered_data[:, 1], filtered_data[:, 2], alpha=0.6, label=variant_label)
        
        plt.xscale('log')
        plt.yscale('log')
        
        if i == 0:
            plt.ylabel('Request + Response Time (ms)')
        
        if i == 2:
            plt.xlabel('Size of Object Transferred (bytes)')
        
        if request_count == 1:
            title = '1 request per page'
        elif request_count == 2:
            title = '2 requests per page'
        elif request_count == 4:
            title = '3-4 requests per page'
        elif request_count == 8:
            title = '5-8 requests per page'
        else:
            title = '9+ requests per page'
        
        plt.title(title)
        
        if i == 0:
            plt.legend(loc='best')
    
    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'figure8-comparison.png'), dpi=300)
    plt.show()

if __name__ == '__main__':
    main()