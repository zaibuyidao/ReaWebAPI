# ReaWebAPI

- 修复 Windows、macOS、Linux 上多窗口 Lua/EEL2 消息长期得不到投递的问题，跨窗口轮转发送，并在工程采样后为工作线程输出、请求分发和后端消息分别保留有界调度时间。
