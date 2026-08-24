"""PYNQ 端实时频谱仪控制、DMA 搬运、UDP 音频接收与交互显示程序。

作者：Ethereal

本程序同时管理三种工作模式：射频高频零跨度分析、射频低频基带分析，以及
48 kHz UDP 音频分析。PL 侧 DDC 和 FFT IP 通过 AXI4-Lite 配置，频谱结果
通过 AXI DMA 写入连续物理内存；Python 端完成 dB 换算、指数平均、主峰追踪
和 Matplotlib/ipywidgets 显示。

运行环境为 PYNQ 的 Jupyter Notebook。使用前应把 FFT.bit、FFT.hwh 与本程序
放在同一工作目录，并确认 UDP 12345 端口未被其他进程占用。
"""

try:
    get_ipython().run_line_magic("matplotlib", "inline")
except NameError:
    # 允许静态检查器或普通 Python 解释器导入文件；硬件访问仍要求 PYNQ 环境。
    pass

from pynq import Overlay, allocate, Clocks
import numpy as np
import matplotlib.pyplot as plt
from IPython.display import display
import ipywidgets as widgets
import asyncio
import socket
import traceback

# -----------------------------------------------------------------------------
# 1. 硬件初始化与射频/音频 DMA 缓冲区分配
# -----------------------------------------------------------------------------
overlay = Overlay("FFT.bit") 
Clocks.fclk0_mhz = 100.0 

POINTS_RF = 4096
NFFT_LOG2_RF = 12
FS_RF = 32e6  

POINTS_AUDIO = 4096
NFFT_LOG2_AUDIO = 12
FS_AUDIO = 48000.0

# allocate 返回 DMA 可访问的连续物理内存；三块缓冲区不能用普通 NumPy 数组替代。
rx_buffer_rf = allocate(shape=(POINTS_RF,), dtype=np.uint32)
rx_buffer_audio = allocate(shape=(POINTS_AUDIO,), dtype=np.uint32)
tx_buffer_audio = allocate(shape=(POINTS_AUDIO,), dtype=np.int32)

# UDP 音频帧按 4096 个 int16 小端采样发送，即每个有效数据报 8192 字节。
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", 12345))
sock.setblocking(False) 

# -----------------------------------------------------------------------------
# 2. 工作模式状态机与交互控件
# -----------------------------------------------------------------------------
SPAN_MAP = {
    "Span: 1.0 MHz | RBW: 244 Hz (32x 抽取)": 32,
    "Span: 250 kHz | RBW: 61.0 Hz (128x 抽取)": 128,
    "Span: 125 kHz | RBW: 30.5 Hz (256x 抽取)": 256,
    "Span: 31.2 kHz | RBW: 7.6 Hz (1024x 抽取)": 1024,
    "Span: 15.6 kHz | RBW: 3.8 Hz (2048x 抽取)": 2048,
    "Span: 7.8 kHz | RBW: 1.9 Hz (4096x 深度抽取)": 4096,
    "Span: 3.9 kHz | RBW: 0.95 Hz (8192x 极限抽取)": 8192,
    "Span: 1.95 kHz | RBW: 0.47 Hz (16384x 终极抽取)": 16384
}

global_state = {
    'mode': 'LOW',           
    'decim_target': 256,     
    'locked_freq': 0.0,      
    'running': True          
}

high_btn = widgets.Button(description="🎯 射频高频模式", button_style='success', icon='crosshairs', layout=widgets.Layout(width='150px'))
low_btn = widgets.Button(description="🔬 射频低频直采", button_style='info', icon='wave-square', layout=widgets.Layout(width='150px'))
audio_btn = widgets.Button(description="🎵 UDP 音频分析", button_style='warning', icon='music', layout=widgets.Layout(width='160px'))
stop_btn = widgets.Button(description="🛑 停止系统", button_style='danger', layout=widgets.Layout(width='120px'))

center_freq_input = widgets.BoundedFloatText(value=1.000, min=0.0, max=16.0, step=0.01, description='🎯 射频载波(MHz):', layout=widgets.Layout(width='220px'))
span_dropdown = widgets.Dropdown(options=list(SPAN_MAP.keys()), value="Span: 125 kHz | RBW: 30.5 Hz (256x 抽取)", description='⚙️ 射频频宽:', layout=widgets.Layout(width='380px'))
status_html = widgets.HTML(value="<div style='padding: 5px; font-size: 14px; color: #00FFCC;'><b>系统状态:</b> 全域分析引擎就绪...</div>")

def on_high(b): 
    """切换到以指定 NCO 频率为中心的射频零跨度模式。"""
    global_state['mode'] = 'HIGH'; global_state['locked_freq'] = center_freq_input.value * 1e6
    center_freq_input.disabled = False; span_dropdown.disabled = False
def on_low(b): 
    """切换到从直流开始观察的低频基带模式。"""
    global_state['mode'] = 'LOW'; global_state['locked_freq'] = 0.0
    center_freq_input.disabled = False; span_dropdown.disabled = False
def on_audio(b): 
    """切换到 UDP 音频模式，并锁定射频配置控件。"""
    global_state['mode'] = 'AUDIO'
    center_freq_input.disabled = True; span_dropdown.disabled = True 
def on_span(change): 
    """把用户选择的观察带宽映射为 DDC 抽取倍数。"""
    if change['name'] == 'value': global_state['decim_target'] = SPAN_MAP[change['new']]
def on_freq(change): 
    """更新 NCO 中心频率；修改频率时自动进入高频模式。"""
    if change['name'] == 'value': 
        global_state['mode'] = 'HIGH'; global_state['locked_freq'] = change['new'] * 1e6
        center_freq_input.disabled = False; span_dropdown.disabled = False
def on_stop(b): 
    """请求异步引擎安全退出，并禁止重复触发停止操作。"""
    global_state['running'] = False; stop_btn.description = "已安全停止"; stop_btn.disabled = True

high_btn.on_click(on_high); low_btn.on_click(on_low); audio_btn.on_click(on_audio)
span_dropdown.observe(on_span); center_freq_input.observe(on_freq); stop_btn.on_click(on_stop)

row1 = widgets.HBox([high_btn, center_freq_input, low_btn])
row2 = widgets.HBox([audio_btn, span_dropdown, stop_btn])
display(widgets.VBox([row1, row2, status_html]))

# -----------------------------------------------------------------------------
# 3. 频谱曲线、峰值标记和状态显示初始化
# -----------------------------------------------------------------------------
plt.ioff() 
plt.rcParams['figure.facecolor'] = '#121212'
fig, ax = plt.subplots(figsize=(10, 4.5))
ax.set_facecolor('#1E1E1E')

line, = ax.plot(np.linspace(-1, 1, POINTS_RF), np.zeros(POINTS_RF) - 100, color='#00FFCC', linewidth=1.2)
marker_dot, = ax.plot([], [], 'ro', markersize=6, zorder=5) 
marker_text = ax.text(0.98, 0.95, '', transform=ax.transAxes, ha='right', va='top', 
                      color='#00FFCC', fontsize=11, fontfamily='monospace',
                      bbox=dict(facecolor='#111111', alpha=0.8, edgecolor='#00FFCC', boxstyle='round,pad=0.5'))

ax.tick_params(colors='white')
ax.grid(True, linestyle='--', alpha=0.3, color='gray')
ax.set_ylabel("Magnitude (dB)", color='white')

display_handle = display(fig, display_id=True)
plt.ion()

# -----------------------------------------------------------------------------
# 4. PL 配置、DMA 传输、频谱换算与实时渲染主循环
# -----------------------------------------------------------------------------
async def live_multi_engine():
    """持续响应模式变化并驱动 PL 数据通路，直到 running 被清零。

    射频模式由 ADC 流直接触发接收 DMA；音频模式则先把 UDP int16 帧写入发送
    DMA，再接收 FFT 功率谱。跨射频/音频模式时重新下载 Overlay，清除 HLS IP
    内部静态状态和可能残留的 AXI 流数据。finally 分支负责释放套接字及 DMA
    连续内存，避免 Notebook 重复运行后资源泄漏。
    """
    global overlay
    current_mode = None
    current_decim = None
    current_nco = -1.0
    
    avg_mag_rf = np.zeros(POINTS_RF) - 50
    avg_mag_audio = np.zeros(POINTS_AUDIO // 2) - 100
    ALPHA = 0.6  
    
    audio_buffer = bytearray()
    BYTES_PER_FRAME = POINTS_AUDIO * 2 
    
    l_ddc = overlay.ddc_processor_0
    l_fft = overlay.fft_spectrum_analyzer_0
    l_dma = overlay.axi_dma_0
    l_switch = overlay.axis_switch_0
    l_gpio = overlay.axi_gpio_0

    try:
        while global_state['running']:
            await asyncio.sleep(0.01) 
            
            target_mode = global_state['mode']
            target_decim = global_state['decim_target']
            target_nco = global_state['locked_freq']
            
            need_reconfig = (current_mode != target_mode or 
                             current_decim != target_decim or 
                             current_nco != target_nco)
                
            if need_reconfig:
                if current_mode is not None and \
                   ((current_mode == 'AUDIO' and target_mode in ['HIGH', 'LOW']) or \
                    (current_mode in ['HIGH', 'LOW'] and target_mode == 'AUDIO')):
                    
                    status_html.value = "<div style='color: yellow;'>⚙️ 正在执行 FPGA 物理管线深度硬复位，请稍候...</div>"
                    display_handle.update(fig)
                    
                    overlay.download()
                    Clocks.fclk0_mhz = 100.0
                    
                    l_ddc = overlay.ddc_processor_0
                    l_fft = overlay.fft_spectrum_analyzer_0
                    l_dma = overlay.axi_dma_0
                    l_switch = overlay.axis_switch_0
                    l_gpio = overlay.axi_gpio_0
                    
                    await asyncio.sleep(0.1) 

                current_mode = target_mode
                current_decim = target_decim
                current_nco = target_nco
                
                l_ddc.write(0x00, 0x00); l_fft.write(0x00, 0x00)
                
                if current_mode == 'AUDIO':
                    l_switch.write(0x40, 0)
                    l_switch.write(0x00, 2)
                    l_fft.register_map.ddc_en = 0  
                    l_ddc.register_map.bypass = 1
                    audio_buffer.clear() 
                    
                    l_fft.register_map.window_type = 1
                    l_fft.register_map.nfft_log2 = NFFT_LOG2_AUDIO
                    
                    l_ddc.register_map.nfft_log2 = NFFT_LOG2_AUDIO
                    
                    l_gpio.write(0x00, POINTS_AUDIO)
                    
                    current_x_axis = np.arange(POINTS_AUDIO // 2) * (FS_AUDIO / POINTS_AUDIO) / 1e3
                    ax.set_xlim(0, FS_AUDIO/2 / 1e3)
                    ax.set_ylim(-100, 20)
                    ax.set_title("UDP Network Audio Auto-Zoom | Range: 0-24 kHz", color='#FFAA00')
                    ax.set_xlabel("Audio Frequency (kHz)", color='white')
                    line.set_color('#FFAA00') 
                    
                    avg_mag_audio = np.zeros(POINTS_AUDIO // 2) - 100
                    line.set_data(current_x_axis, avg_mag_audio)
                    marker_dot.set_data([], [])
                    marker_text.set_text("Waiting for packets...")
                    status_html.value = f"<div style='color: #FFAA00;'>⏳ 已切换至音频模式({POINTS_AUDIO}点)，正在监听 UDP 端口 12345...</div>"
                    display_handle.update(fig)
                    
                else:
                    l_switch.write(0x40, 1) 
                    l_switch.write(0x00, 2)
                    l_fft.register_map.ddc_en = 1 
                    
                    l_fft.register_map.window_type = 1
                    l_fft.register_map.nfft_log2 = NFFT_LOG2_RF
                    l_gpio.write(0x00, POINTS_RF)
                    
                    nco_step_val = int((current_nco / FS_RF) * (2**32)) & 0xFFFFFFFF
                    l_ddc.register_map.bypass = 0
                    l_ddc.register_map.decimation = current_decim
                    l_ddc.register_map.gain_shift = int(np.log2(current_decim))
                    l_ddc.register_map.nco_step = nco_step_val
                    
                    l_ddc.register_map.nfft_log2 = NFFT_LOG2_RF
                    
                    current_fs_ddc = FS_RF / current_decim
                    if current_mode == 'HIGH':
                        current_x_axis = np.linspace(-current_fs_ddc/2, current_fs_ddc/2, POINTS_RF) / 1e6
                    else:
                        current_x_axis = np.linspace(-current_fs_ddc/2, current_fs_ddc/2, POINTS_RF) / 1e3
                    line.set_color('#FF00FF') 
                    avg_mag_rf = np.zeros(POINTS_RF) - 50 
                    status_html.value = f"<div style='color: #00FFCC;'>⚙️ 正在重新配置射频分析管线({POINTS_RF}点)...</div>"
                    display_handle.update(fig)

            # ==========================================
            # 分支 1：音频解算链路 (加入动态放大跟随)
            # ==========================================
            if current_mode == 'AUDIO':
                has_valid_packet = False
                try:
                    while True: 
                        chunk, addr = sock.recvfrom(65535)
                        if len(chunk) == BYTES_PER_FRAME:
                            audio_buffer.extend(chunk)
                            has_valid_packet = True
                except BlockingIOError:
                    pass

                if has_valid_packet and len(audio_buffer) >= BYTES_PER_FRAME:
                    frame_bytes = audio_buffer[:BYTES_PER_FRAME]
                    audio_buffer.clear() 

                    audio_data = np.frombuffer(frame_bytes, dtype=np.int16)
                    tx_buffer_audio[:] = np.int32(audio_data)
                    tx_buffer_audio.flush() 
                    
                    l_ddc.write(0x00, 1) 
                    l_fft.write(0x00, 1) 
                    
                    if not l_dma.sendchannel.running:
                        l_dma.sendchannel.start()
                    if not l_dma.recvchannel.running:
                        l_dma.recvchannel.start()
                        
                    l_dma.sendchannel.transfer(tx_buffer_audio)
                    l_dma.recvchannel.transfer(rx_buffer_audio)
                    
                    while not l_dma.sendchannel.idle or not l_dma.recvchannel.idle:
                        await asyncio.sleep(0.001)
                    
                    rx_buffer_audio.invalidate()
                    
                    power_audio = np.maximum(np.array(rx_buffer_audio).view(np.float32)[:POINTS_AUDIO // 2], 1e-12)
                    REF_POWER = (32768.0 * (POINTS_AUDIO / 2.0)) ** 2
                    current_db = 10 * np.log10(power_audio / REF_POWER)
                    avg_mag_audio = ALPHA * current_db + (1 - ALPHA) * avg_mag_audio
                    
                    line.set_data(current_x_axis, avg_mag_audio)
                    
                    # 音频主峰追踪：以峰值为中心建立 5 kHz 局部观察窗。
                    peak_idx = np.argmax(avg_mag_audio)
                    peak_y = avg_mag_audio[peak_idx]
                    peak_freq_khz = current_x_axis[peak_idx]
                    
                    # 设定音频局部视窗宽度为 5 kHz (可以根据喜好修改)
                    window_width = 5.0
                    actual_max_limit = (FS_AUDIO / 2) / 1e3 # 24.0 kHz
                    
                    x_min = max(0.0, peak_freq_khz - window_width / 2)
                    x_max = min(actual_max_limit, peak_freq_khz + window_width / 2)
                    
                    # 边界视野补偿（顶到左侧或右侧时，维持 5kHz 宽度）
                    if x_min == 0.0: 
                        x_max = min(actual_max_limit, window_width)
                    elif x_max == actual_max_limit: 
                        x_min = max(0.0, actual_max_limit - window_width)
                    
                    ax.set_xlim(x_min, x_max)
                    
                    # 动态 Y 轴缩放：只计算当前视野内的数据
                    idx_min = max(0, int((x_min / actual_max_limit) * (POINTS_AUDIO // 2 - 1)))
                    idx_max = min(POINTS_AUDIO // 2 - 1, int((x_max / actual_max_limit) * (POINTS_AUDIO // 2 - 1)))
                    local_max = np.max(avg_mag_audio[idx_min : idx_max]) if idx_max > idx_min else 0
                    
                    # 音频底噪一般较低，设置 -100 为底，最高点留出 20dB 空间给 HUD
                    ax.set_ylim(-100, local_max + 20)
                    
                    ax.set_title(f"UDP Audio Auto Zoom | Range: 0-24 kHz (Peak: {peak_freq_khz:.2f} kHz)", color='#FFAA00')
                    
                    marker_dot.set_data([peak_freq_khz], [peak_y])
                    marker_text.set_text(f"Audio Pk: {peak_freq_khz:.2f} kHz\nMag: {peak_y:.1f} dBFS")
                    status_html.value = f"<div style='color: #FFAA00;'>✅ 实时音频解算中 | UDP 接收正常 | 追踪主峰: {peak_freq_khz:.2f} kHz</div>"
                    
                    display_handle.update(fig)

            # ==========================================
            # 分支 2：射频解算链路
            # ==========================================
            else:
                l_ddc.write(0x00, 0x00); l_fft.write(0x00, 0x00)
                
                if not l_dma.recvchannel.running:
                    l_dma.recvchannel.start()
                    
                l_dma.recvchannel.transfer(rx_buffer_rf)
                l_ddc.write(0x00, 0x81); l_fft.write(0x00, 0x81)
                
                while not l_dma.recvchannel.idle:
                    await asyncio.sleep(0.005)
                
                rx_buffer_rf.invalidate() 
                power_data = np.maximum(np.array(rx_buffer_rf).view(np.float32), 1e-10)
                current_db = np.fft.fftshift(10 * np.log10(power_data))
                
                c_idx = POINTS_RF // 2
                current_db[c_idx - 3 : c_idx + 4] = np.min(current_db) 
                
                avg_mag_rf = ALPHA * current_db + (1 - ALPHA) * avg_mag_rf
                line.set_data(current_x_axis, avg_mag_rf)
                
                noise_floor = np.min(avg_mag_rf)
                span_khz = current_fs_ddc / 1e3
                
                if current_mode == 'HIGH':
                    span_mhz = current_fs_ddc / 1e6
                    ax.set_xlim(-span_mhz/2, span_mhz/2)
                    ax.set_ylim(noise_floor - 5, np.max(avg_mag_rf) + 20) 
                    ax.set_title(f"RF Zero-Span (Centered: {current_nco/1e6:.4f} MHz) | Span: {span_khz:.1f} kHz", color='#00FFCC')
                    ax.set_xlabel("Frequency Offset (MHz)", color='white')
                    
                    peak_idx = np.argmax(avg_mag_rf)
                    peak_y = avg_mag_rf[peak_idx]
                    peak_x_offset = current_x_axis[peak_idx]
                    abs_freq_mhz = (current_nco / 1e6) + peak_x_offset
                    
                    marker_dot.set_data([peak_x_offset], [peak_y])
                    marker_text.set_text(f"Mk1: {abs_freq_mhz:.4f} MHz\nAmp: {peak_y:.1f} dB")
                    status_html.value = f"<div style='color: #00FF00;'>✅ 射频实时观测中 | 载波: {current_nco/1e6:.4f} MHz | 抽取率: {current_decim}X</div>"

                elif current_mode == 'LOW':
                    hardware_max_khz = span_khz / 2.0
                    actual_max_limit = min(72.0, hardware_max_khz)
                    
                    search_bins = int(actual_max_limit / (span_khz / POINTS_RF))
                    search_bins = min(search_bins, POINTS_RF - c_idx)
                    
                    positive_half = avg_mag_rf[c_idx : c_idx + search_bins]
                    local_peak_idx = np.argmax(positive_half)
                    peak_freq_khz = current_x_axis[c_idx + local_peak_idx]
                    peak_y = avg_mag_rf[c_idx + local_peak_idx]
                    
                    window_width = 10.0
                    x_min = max(0.0, peak_freq_khz - window_width / 2)
                    x_max = min(actual_max_limit, peak_freq_khz + window_width / 2)
                    
                    if x_min == 0.0: x_max = min(actual_max_limit, window_width)
                    elif x_max == actual_max_limit: x_min = max(0.0, actual_max_limit - window_width)
                    
                    ax.set_xlim(x_min, x_max)
                    
                    idx_min = max(0, int((x_min - current_x_axis[0]) / (current_x_axis[-1] - current_x_axis[0]) * POINTS_RF))
                    idx_max = min(POINTS_RF-1, int((x_max - current_x_axis[0]) / (current_x_axis[-1] - current_x_axis[0]) * POINTS_RF))
                    local_max = np.max(avg_mag_rf[idx_min : idx_max]) if idx_max > idx_min else 0
                    ax.set_ylim(noise_floor - 5, local_max + 20)
                    
                    marker_dot.set_data([peak_freq_khz], [peak_y])
                    marker_text.set_text(f"Mk1: {peak_freq_khz:.3f} kHz\nAmp: {peak_y:.1f} dB")
                    
                    ax.set_title(f"RF Baseband (Range: 0-{actual_max_limit:.0f} kHz)", color='#FF00FF')
                    ax.set_xlabel("Absolute Frequency (kHz)", color='white')
                    status_html.value = f"<div style='color: #00FFFF;'>✅ 射频基带直采 | 追踪主峰: {peak_freq_khz:.3f} kHz | 抽取率: {current_decim}X</div>"
                
                display_handle.update(fig)

    except Exception as e:
        status_html.value = f"<div style='color:red;'><b>发生致命错误:</b> {e}</div>"
        import traceback
        traceback.print_exc()
    finally:
        sock.close()
        try:
            l_ddc.write(0x00, 0x00); l_fft.write(0x00, 0x00)
        except:
            pass
        rx_buffer_rf.free()
        rx_buffer_audio.free()
        tx_buffer_audio.free()
        print("\n✅ 硬件双引擎已安全关闭，释放 DMA 内存与端口资源。")

# 启动全域异步引擎
task = asyncio.ensure_future(live_multi_engine())
