# ReaWebAPI

- Linux 通过事件循环读取 WebKit 入站消息，复用 X11 几何信息并在原生窗口事件到达时使缓存失效，降低多窗口延迟。
- WSL 未提供 DRI3 时，辅助进程使用图像后备缓冲以减少 GTK 绘制停顿，保留显式 GDK_RENDERING 设置。
