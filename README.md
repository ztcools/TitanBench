# titanbench

工业级高性能压测工具，支持 `http/https/tcp/udp`，基于多线程 + epoll 事件驱动，提供实时 CLI 报告与完整统计。

## 阶段八优化说明

- 内存优化
  - 网络命令对象池（无锁 freelist）复用 `Command`，降低压测期间频繁 `new/delete`
  - 热路径容器预留容量，减少运行期扩容
  - RAII 关闭 fd/线程/资源，避免泄漏
- CPU 优化
  - worker 线程按 CPU 核心绑核（Linux）
  - 降低调度抖动与上下文切换
- 网络优化
  - 非阻塞 socket
  - `SO_REUSEADDR` / `SO_REUSEPORT`（平台支持时）
  - TCP `TCP_NODELAY`
- 错误处理与保护
  - 致命信号处理（SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGTERM）
  - `std::terminate` 兜底保护
  - watchdog 超时保护（防死循环）
- 发布能力
  - 静态链接发布（`CMakeLists.txt` 已配置）
  - 生成单文件二进制
  - 支持 `x86_64` / `arm64`（通过 vcpkg triplet）

## 构建方式

### 方式1：一键脚本（推荐）

```bash
./scripts/build.sh
```

输出文件：

- `dist/titanbench-x64-linux` 或 `dist/titanbench-arm64-linux`

### 方式2：Makefile

```bash
make build
make package
```

常用变量：

- `BUILD_TYPE=Release|Debug`
- `TRIPLET=x64-linux|arm64-linux`

示例：

```bash
make build BUILD_TYPE=Release TRIPLET=arm64-linux
```

## 使用说明

```bash
./build/titanbench -c <concurrency> (-n <requests> | -t <seconds>) [options]
```

参数：

- `-c, --concurrency`：并发连接数（必填）
- `-n, --requests`：总请求数模式（与 `-t` 互斥）
- `-t, --time`：持续压测秒数模式（与 `-n` 互斥）
- `-T, --threads`：worker 线程数
- `-p, --protocol`：`http|https|tcp|udp`
- `-h, --host`：目标主机
- `-P, --port`：目标端口
- `--path`：HTTP/HTTPS 路径

## 命令示例

### HTTP 固定请求数

```bash
./build/titanbench -c 1000 -n 500000 -T 8 -p http -h 127.0.0.1 -P 8080 --path /api/ping
```

### HTTPS 持续压测

```bash
./build/titanbench -c 2000 -t 60 -T 16 -p https -h example.com -P 443 --path /
```

### TCP 高并发

```bash
./build/titanbench -c 50000 -t 30 -T 32 -p tcp -h 10.0.0.10 -P 9000
```

### UDP 高并发

```bash
./build/titanbench -c 100000 -t 20 -T 32 -p udp -h 10.0.0.20 -P 7000
```

## 10万+ 并发建议（Linux）

执行前建议：

```bash
ulimit -n 1048576
sysctl -w net.core.somaxconn=65535
sysctl -w net.ipv4.ip_local_port_range="1024 65535"
sysctl -w net.ipv4.tcp_tw_reuse=1
sysctl -w net.core.rmem_max=134217728
sysctl -w net.core.wmem_max=134217728
```

并发配置建议：

- `-T` 通常设置为物理核心数或略低于逻辑核心数
- 压测机与目标机分离，避免互相抢占 CPU
- 先小规模 warm-up，再提升到峰值并发

## 输出报告

- 每秒实时刷新：QPS、完成数、失败数、吞吐
- 结束后输出完整报告：
  - 目标信息
  - 压测参数
  - 总体统计
  - 延迟统计（avg/min/max/P50/P95/P99）
  - QPS统计
  - 成功率
  - 错误统计
  - 网络流量

## 交叉架构说明

- 当前主机编译默认跟随 `uname -m`
- 强制指定：
  - `TRIPLET=x64-linux`
  - `TRIPLET=arm64-linux`
- 需在本机 vcpkg 安装对应 triplet 依赖后再构建
