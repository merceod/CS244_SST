#!/usr/bin/env python3
"""
HTTP Performance Log Plotter
Creates plots similar to Figure 8 from the SST paper with separate subplots.

Usage:
python plot_logs.py --files log1.txt log2.txt --labels "HTTP/1.0 Serial" "HTTP/1.1 Pipelined"
"""

import re
import argparse
import matplotlib.pyplot as plt
import numpy as np
from collections import defaultdict

def parse_log_file(filename):
    """
    Parse a log file and extract page performance data.
    
    Returns:
        list of dicts: Each dict contains 'requests', 'time_ms', 'total_size'
    """
    pages = []
    
    # Regex pattern to match page result lines
    pattern = r'Page \d+ \((\d+) requests\): ([\d.]+) ms .* - Total size: (\d+) bytes'
    
    try:
        with open(filename, 'r') as f:
            for line in f:
                match = re.search(pattern, line)
                if match:
                    requests = int(match.group(1))
                    time_ms = float(match.group(2))
                    total_size = int(match.group(3))
                    
                    pages.append({
                        'requests': requests,
                        'time_ms': time_ms,
                        'total_size': total_size
                    })
    except FileNotFoundError:
        print(f"Warning: File {filename} not found")
        return []
    
    return pages

def group_by_request_count(pages):
    """
    Group pages by request count ranges like in Figure 8.
    
    Returns:
        dict: Key is group name, value is list of pages
    """
    groups = {
        '1 request per page': [],
        '2 requests per page': [],
        '3-4 requests per page': [],
        '5-8 requests per page': [],
        '9+ requests per page': []
    }
    
    for page in pages:
        req_count = page['requests']
        if req_count == 1:
            groups['1 request per page'].append(page)
        elif req_count == 2:
            groups['2 requests per page'].append(page)
        elif 3 <= req_count <= 4:
            groups['3-4 requests per page'].append(page)
        elif 5 <= req_count <= 8:
            groups['5-8 requests per page'].append(page)
        else:  # 9+
            groups['9+ requests per page'].append(page)
    
    return groups

def create_plot(data_sets, labels):
    """
    Create a multi-subplot plot similar to Figure 8.
    
    Args:
        data_sets: List of parsed page data for each log file
        labels: List of labels for each data set
    """
    # Create figure with 5 subplots arranged horizontally
    fig, axes = plt.subplots(1, 5, figsize=(20, 6), sharey=True)
    
    # Colors and markers for different implementations
    colors = ['green', 'red', 'magenta', 'blue', 'orange', 'purple', 'brown', 'pink']
    markers = ['+', 'o', '*', 's', '^', 'v', 'D', 'x']
    
    # Group names for subplots
    group_names = ['1 request per page', '2 requests per page', '3-4 requests per page', 
                   '5-8 requests per page', '9+ requests per page']
    
    # Process each subplot
    for subplot_idx, (ax, group_name) in enumerate(zip(axes, group_names)):
        
        # Plot each data set on this subplot
        for dataset_idx, (pages, label) in enumerate(zip(data_sets, labels)):
            color = colors[dataset_idx % len(colors)]
            marker = markers[dataset_idx % len(markers)]
            
            # Group pages by request count
            groups = group_by_request_count(pages)
            group_pages = groups[group_name]
            
            if not group_pages:
                continue
            
            # Extract data for this group
            sizes = [page['total_size'] for page in group_pages]
            times = [page['time_ms'] for page in group_pages]
            
            # Plot the data points with appropriate marker styles
            # Some markers ('+', 'x') are line-based and can't be hollow
            if marker in ['+', 'x']:
                ax.scatter(sizes, times, c=color, marker=marker, s=50, alpha=0.7, 
                          linewidths=0.8, label=label)
            else:
                # Shape-based markers can be made hollow
                ax.scatter(sizes, times, facecolors='none', edgecolors=color, 
                          marker=marker, s=50, alpha=0.7, linewidths=0.8,
                          label=label)
        
        # Customize each subplot
        ax.set_xscale('log')
        ax.set_yscale('log')
        ax.set_title(group_name, fontsize=12)
        ax.grid(True, alpha=0.3)
        
        # Set consistent axis limits for all subplots
        ax.set_xlim(100, 100000)
        ax.set_ylim(50, 5000)
        
        # Customize tick labels to match Figure 8
        ax.set_xticks([128, 1000, 8000, 64000])
        ax.set_xticklabels(['128B', '1K', '8K', '64K'])
        
        # Set y-ticks for all subplots to ensure consistent formatting
        ax.set_yticks([60, 100, 200, 400, 600, 1000, 2000, 4000])
        ax.set_yticklabels(['60ms', '100ms', '200ms', '400ms', '600ms', '1s', '2s', '4s'])
        
        # Only show y-tick labels on the leftmost subplot
        if subplot_idx == 0:
            ax.set_ylabel('Request + Response Time', fontsize=12)
        else:
            # Hide y-tick labels for other subplots using tick_params
            ax.tick_params(axis='y', labelleft=False)
        
        # Set x-label for all subplots
        ax.set_xlabel('Total Bytes Transferred', fontsize=10)
    
    # Add overall title
    fig.suptitle('Web workload comparing HTTP implementations', fontsize=16, y=0.98)
    
    # Add legend outside the plot area
    # Get handles and labels from the first subplot that has data
    handles, legend_labels = None, None
    for ax in axes:
        h, l = ax.get_legend_handles_labels()
        if h:  # If this subplot has data
            handles, legend_labels = h, l
            break
    
    if handles:
        fig.legend(handles, legend_labels, bbox_to_anchor=(1.02, 0.5), 
                  loc='center left', fontsize=12)
    
    plt.tight_layout()
    return fig

def print_statistics(data_sets, labels):
    """Print summary statistics for each data set."""
    print("\n" + "="*60)
    print("SUMMARY STATISTICS")
    print("="*60)
    
    for pages, label in zip(data_sets, labels):
        if not pages:
            print(f"\n{label}: No data")
            continue
            
        times = [page['time_ms'] for page in pages]
        sizes = [page['total_size'] for page in pages]
        requests = [page['requests'] for page in pages]
        
        print(f"\n{label}:")
        print(f"  Pages processed: {len(pages)}")
        print(f"  Average page load time: {np.mean(times):.2f} ms")
        print(f"  Median page load time: {np.median(times):.2f} ms")
        print(f"  Average page size: {np.mean(sizes):.0f} bytes")
        print(f"  Average requests per page: {np.mean(requests):.1f}")
        
        # Group statistics
        groups = group_by_request_count(pages)
        print(f"  Request count distribution:")
        for group_name in ['1 request per page', '2 requests per page', '3-4 requests per page', 
                          '5-8 requests per page', '9+ requests per page']:
            count = len(groups[group_name])
            if count > 0:
                avg_time = np.mean([p['time_ms'] for p in groups[group_name]])
                print(f"    {group_name}: {count} pages (avg: {avg_time:.1f} ms)")

def main():
    parser = argparse.ArgumentParser(description='Plot HTTP performance logs similar to SST paper Figure 8')
    parser.add_argument('--files', nargs='+', required=True,
                        help='Log files to process')
    parser.add_argument('--labels', nargs='+', required=True,
                        help='Labels for each log file')
    parser.add_argument('--output', default='http_performance_comparison.png',
                        help='Output filename for the plot')
    parser.add_argument('--show', action='store_true',
                        help='Show the plot interactively')
    parser.add_argument('--stats', action='store_true',
                        help='Print detailed statistics')
    
    args = parser.parse_args()
    
    if len(args.files) != len(args.labels):
        print("Error: Number of files must match number of labels")
        return
    
    # Parse all log files
    data_sets = []
    for filename in args.files:
        print(f"Parsing {filename}...")
        pages = parse_log_file(filename)
        data_sets.append(pages)
        print(f"  Found {len(pages)} pages")
    
    if not any(data_sets):
        print("Error: No data found in any log files")
        return
    
    # Print statistics if requested
    if args.stats:
        print_statistics(data_sets, args.labels)
    
    # Create the plot
    print(f"\nCreating plot...")
    fig = create_plot(data_sets, args.labels)
    
    # Save the plot
    fig.savefig(args.output, dpi=300, bbox_inches='tight')
    print(f"Plot saved as {args.output}")
    
    # Show interactively if requested
    if args.show:
        plt.show()

if __name__ == "__main__":
    main()