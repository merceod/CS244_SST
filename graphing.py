import matplotlib.pyplot as plt
import numpy as np

requests = []
for _ in range(300):
    requests.append([])

with open('output/base_logs2.txt', 'r') as f:
    for line in f:
        if line.startswith('Page') and "requests" in line and ":" in line:
            # Extract second, third and fifth column
            line = line.split()
            id = int(line[1])
            num_req = int(line[2][1:])
            time = float(line[4])
            size = int(line[-2])
            requests[num_req].append((time,size))


def plot_horizontal_graphs(data_lists, titles, xlabel='Size', ylabel='Time', figsize=(15, 4)):
    """
    Create multiple graphs in a horizontal line from lists of pairs.
    
    Parameters:
    - data_lists: List of lists, where each inner list contains (x, y) pairs
    - titles: List of titles for each subplot
    - xlabel: Label for x-axis
    - ylabel: Label for y-axis
    - figsize: Figure size as (width, height)
    """
    n_plots = len(data_lists)
    
    # Create figure and subplots
    fig, axes = plt.subplots(1, n_plots, figsize=figsize)
    
    # Handle case with only one subplot
    if n_plots == 1:
        axes = [axes]
    
    # Plot data on each subplot
    for i, (data, title) in enumerate(zip(data_lists, titles)):
        if data:  # Only plot if data exists
            y_values, x_values = zip(*data)
            axes[i].scatter(x_values, y_values, marker='x', label="HTTP 1.0 serial")
            axes[i].set_title(title)
            axes[i].set_xlabel(xlabel)
            axes[i].set_xscale('log')
            axes[i].legend()
            
            # Only add y-label to the first subplot
            if i == 0:
                axes[i].set_ylabel(ylabel)
    
    plt.tight_layout()
    return fig, axes

# Example usage:
# Group requests by count
request_groups = [
    requests[1],
    requests[2],
    requests[3],
    requests[4]
]
titles = ['1 request', '2 requests', '4 requests', '8 requests']
fig, axes = plot_horizontal_graphs(request_groups, titles)
# plt.savefig('request_comparison.png')
plt.show()
