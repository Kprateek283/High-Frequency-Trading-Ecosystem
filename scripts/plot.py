import matplotlib.pyplot as plt
import numpy as np

def plot_latency_scaling():
    threads = ['1 Thread', '2 Threads', '4 Threads', '8 Threads']
    latencies = [243483071, 1840357, 1422379, 1588183]
    
    # Convert to microseconds (assuming ~4.4 GHz -> 4400 cycles = 1 us)
    # Actually, let's just plot cycles to be consistent with the doc
    
    fig, ax = plt.subplots(figsize=(8, 6))
    bars = ax.bar(threads, latencies, color=['#e74c3c', '#f1c40f', '#2ecc71', '#e67e22'])
    
    ax.set_yscale('log')
    ax.set_title('End-to-End Latency at 10,000,000 msgs/sec', fontsize=14, pad=20)
    ax.set_ylabel('CPU Cycles (Log Scale)', fontsize=12)
    ax.set_xlabel('SO_REUSEPORT Gateway Shards', fontsize=12)
    
    # Add exact values on top of bars
    for bar in bars:
        yval = bar.get_height()
        label = f"{yval:,}"
        if yval > 200000000:
            label = "243M (Buffer Saturation)"
        elif yval == 1422379:
            label = "1.42M (Optimal)"
        else:
            label = f"{yval/1000000:.2f}M"
            
        ax.text(bar.get_x() + bar.get_width()/2, yval * 1.2, label, ha='center', va='bottom', fontsize=10, fontweight='bold')
        
    ax.grid(axis='y', linestyle='--', alpha=0.7)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    
    plt.tight_layout()
    plt.savefig('docs/images/latency_scaling.png', dpi=300, bbox_inches='tight')
    print("Generated latency_scaling.png")

def plot_cycle_attribution():
    labels = ['epoll_wait()', 'read()', 'Decode', 'Validation', 'Enqueue']
    cycles = [1157, 124, 82, 27, 258]
    colors = ['#3498db', '#2980b9', '#2ecc71', '#27ae60', '#f1c40f']
    
    fig, ax = plt.subplots(figsize=(8, 6))
    
    # Explode the kernel vs app
    explode = (0.05, 0.05, 0, 0, 0)
    
    ax.pie(cycles, labels=labels, autopct='%1.1f%%', startangle=90, colors=colors, explode=explode,
           textprops={'fontsize': 11})
    
    ax.set_title('Gateway CPU Cycle Attribution (1,650 Cycles Total)', fontsize=14, pad=20)
    
    # Draw a circle at the center to make it a donut chart
    centre_circle = plt.Circle((0,0),0.70,fc='white')
    fig.gca().add_artist(centre_circle)
    
    # Add a legend
    ax.text(0, 0, 'Kernel\n77%', ha='center', va='center', fontsize=16, fontweight='bold', color='#2980b9')
    
    plt.tight_layout()
    plt.savefig('docs/images/cycle_attribution.png', dpi=300, bbox_inches='tight')
    print("Generated cycle_attribution.png")

import os
os.makedirs('docs/images', exist_ok=True)
plot_latency_scaling()
plot_cycle_attribution()
