# Future permissions / 后续权限设计

This is a design note, not an implemented v0.1.7 contract. Current Apps are trusted and can call the complete standard REAPER mirror and privileged native file services. Browser origin isolation protects Web storage; it does not sandbox Native APIs. No consent dialog, capability grant or manifest `permissions` field is implemented.

v0.1.7 只保留设计：可信应用可调用完整标准镜像和已有宿主能力。Manifest 校验不能形成安全边界，也不加入许可弹窗。未来出现未知来源应用分发后，再单独评估下面的方案。

## Requirements for a future design

- Identify an App by verified package identity and origin, and bind grants to its installed version/source. A directory name alone is insufficient to establish publisher trust.
- Describe meaningful capabilities such as project read/write, filesystem locations, external URLs and audio analysis. Audit equivalent standard REAPER entry points so a restricted namespace cannot be bypassed through the mirror.
- Enforce decisions in the native dispatcher and worker operations, including batches, indirect paths, symlinks, handles and cross-window requests. JavaScript checks alone are insufficient.
- Define grant persistence, revocation, migration for existing trusted Apps, and behavior for already-running operations. Make denial errors and diagnostics consistent.
- Validate the model with adversarial Apps before claiming isolation. Keep privileged developer mode explicit and distinguish browser network/storage policy from REAPER capabilities.

Those decisions require their own threat model, compatibility policy and release scope. They do not change v0.1.7 behavior.
