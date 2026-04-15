#!/usr/bin/env bash
set -euo pipefail

echo "[titanbench] 安装系统依赖（cmake >= 3.30 / g++ / git / pkg-config）"

sudo apt update
sudo apt install -y \
  git \
  g++ \
  curl \
  ca-certificates \
  pkg-config

# 安装新版 cmake（保证 vcpkg 不炸）
curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | sudo gpg --dearmor -o /usr/share/keyrings/kitware-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/kitware-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/kitware.list

sudo apt update
sudo apt install -y cmake