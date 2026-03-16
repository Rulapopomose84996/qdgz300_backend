# WSL 交叉编译指南

> 适用环境：WSL Ubuntu  
> 默认源码目录：`/mnt/d/WorkSpace/Company/Tower/qdgz300_backend`

## 1. 前提

- 已安装 `cmake`、`ninja`、`aarch64-linux-gnu-gcc`、`aarch64-linux-gnu-g++`
- 共享离线包目录已就绪
- 使用脚本 `scripts/build/build_wsl_cross.sh` 作为正式入口

## 2. 默认构建

```bash
cd /mnt/d/WorkSpace/Company/Tower/qdgz300_backend
bash scripts/build/build_wsl_cross.sh
```

默认行为：

- 构建目录：`build_wsl_cross_dev`
- 构建类型：`Debug`
- 测试：默认不执行
- GPU：默认关闭
- 依赖缓存目录：`/mnt/d/WorkSpace/ThirdPartyCache/qdgz300_backend/build/wsl-aarch64`

## 3. 常见环境变量

```bash
cd /mnt/d/WorkSpace/Company/Tower/qdgz300_backend
QDGZ300_BUILD_TYPE=Release \
QDGZ300_RUN_TESTS=ON \
QDGZ300_BUILD_TESTING=ON \
bash scripts/build/build_wsl_cross.sh
```

可选变量：

- `QDGZ300_BUILD_DIR`
- `QDGZ300_TOOLCHAIN_FILE`
- `QDGZ300_OFFLINE_DEPS_DIR`
- `QDGZ300_DEPS_ROOT`
- `QDGZ300_RUN_TESTS`
- `QDGZ300_ENABLE_GPU`
- `QDGZ300_ENABLE_PROTOBUF`

## 4. 独立测试入口

```bash
cd /mnt/d/WorkSpace/Company/Tower/qdgz300_backend
bash scripts/test/test_unit.sh build_wsl_cross_dev
```

## 5. 注意事项

- 交叉构建默认只验证编译和单元测试，不替代服务器权威验证。
- WSL 构建和服务器原生构建使用不同的共享依赖缓存目录。
- 当前正式流程不再以旧 `deps_offline/` 文档为准。
