# App trust model / 应用信任模型

ReaWebAPI runs trusted local Apps. An App can call the standard REAPER APIs and native file services available to the extension.

Each App has its own browser profile and local resource origin. This isolates browser storage and cookies. Native API access uses the REAPER process permissions.

The manifest validator checks App metadata and entry paths. Review an App and its source before opening it with ReaWebAPI.

ReaWebAPI 运行受信任的本地应用，应用可调用扩展提供的标准 REAPER API 和原生文件服务。

每个 App 使用独立的浏览器配置与本地资源来源，隔离浏览器存储和 Cookie。原生 API 使用 REAPER 进程的访问权限。

Manifest 校验工具检查应用元数据和入口路径。使用 ReaWebAPI 打开应用前，应确认其来源和代码可信。
