from matplotlib.ticker import MultipleLocator
from brokenaxes import brokenaxes
import matplotlib.pyplot as plt
import numpy as np
 
plt.style.use('seaborn-v0_8-whitegrid')
plt.rcParams.update({
    'font.family': 'Times New Roman',
    'font.size': 12,
    'axes.linewidth': 0.5,
    'grid.linewidth': 0.3,
    'figure.dpi': 300,
})

clusters = [ "RDB time" ]
groups = [ "CPU" , "naïve\nDSA" , "+Alloc." , "+Interl.\n PF" , "+In-Batch\n mixing" , "+Write\n Align" , "+OoO\n Recycle" ]
# data = np.array([ 
#         [ 2.606, 1.317, 1.311, 1.317, 1.139, 1.008, 1.006] ] ).T
data = np.loadtxt('rdb_time_data.txt').reshape(-1,1)       

fig, ax = plt.subplots(figsize=(5, 2.5)) 
 
bar_width = 0.06 
gap_between_bars = 0.01 
gap_between_clusters = 0.1 
 
n_groups = len(groups)
n_clusters = len(clusters) 
cluster_centers = np.arange(n_groups) * (n_clusters * (bar_width + gap_between_bars) + gap_between_clusters)
 
x_positions = []
for i in range(n_clusters):
    x_positions.append(cluster_centers + i * (bar_width + gap_between_bars))
x_positions = np.array(x_positions)
 
colors = ['#6A8DBA'] 
colors.reverse() 

ax.set_ylim( 0.8 , 1.5 ) 
y_max = ax.get_ylim()[1] 
broken_bar_height = 0.9 * y_max
broken_gap = 0.05
for i in range(n_clusters): 
    bars = ax.bar(x_positions[i], data[:, i], 
          width=bar_width,
          color=colors[i], 
          linewidth=0.3,
          label=clusters[i])
    for j in range( n_groups ):
        bar = bars[j]
        height = bar.get_height()  
        delta = - data[j-1,0] + data[j,0] if j > 0 else 0 
        if height > y_max:  
            bar.set_height( broken_bar_height ) 
            ax.bar( x_positions[i,j], y_max - broken_bar_height - broken_gap ,
                bottom = broken_bar_height + broken_gap,
                width=bar_width, 
                color=colors[i],  
                linewidth=0.3 )
            ax.plot([ x_positions[i,j] - bar_width/2, x_positions[i,j] + bar_width/2], 
                [ broken_bar_height , broken_bar_height + broken_gap ], 
                color='black', linewidth=0.5) 
            ax.text(bar.get_x() + bar.get_width() / 2, y_max * 1.04 ,
                   f'{height:.3f}', ha='center', va='top', color='black', fontsize=12, fontweight='bold')
        else :
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + y_max * 0.04 ,
                   f'{delta:+.3f}', ha='center', va='top', color='black', fontsize=12 )
 
ax.set_xticks(cluster_centers + (n_clusters-1)*(bar_width+gap_between_bars)/2)
ax.set_xticklabels(groups , fontsize=11)  
ax.xaxis.grid(False) 
ax.set_xlabel('') 
ax.spines['bottom'].set_color('black') 

ax.yaxis.set_major_locator(MultipleLocator(0.2)) 
ax.yaxis.set_minor_locator(MultipleLocator(0.1))  
ax.grid(axis='y', which='major', linestyle='-', linewidth=0.3, alpha=0.7) 
ax.grid(axis='y', which='minor', linestyle=':', linewidth=0.3, alpha=0.7) 
ax.set_ylabel('RDB time (s)')  
ax.spines['left'].set_color('black') 
 
ax.spines['top'].set_visible(False) 
ax.spines['right'].set_visible(False)  
# for text in legend.get_texts():
#     text.set_verticalalignment('center')

plt.tight_layout()
 
plt.savefig('figure14.png', bbox_inches='tight', transparent=True)
plt.savefig('figure14.pdf', bbox_inches='tight')  
 

# caption: 
# Impact of each LightDSA optimization on RDB save time. 
# Each bar shows the total RDB time after incrementally adding optimizations to the baseline naive DSA implementation. 