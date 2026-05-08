# 最小 KVM VM demo 实验记录

## 1. Hello/KVM 最小闭环

本阶段验证 userspace VMM 的最小生命周期：打开 `/dev/kvm`，创建 VM/vCPU，注册 guest RAM，设置 real-mode vCPU 状态，执行 `KVM_RUN`，处理 `KVM_EXIT_IO` 和 `KVM_EXIT_HLT`。

预期输出：

```text
KVM API version: 12
guest output: Hello, KVM!
KVM_EXIT_HLT
io exits: 12
```
