#!/usr/bin/env pwsh
# M01 测试编译验证脚本
# 用途：验证所有测试文件可以成功编译和运行

param(
    [string]$BuildType = "Release",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "M01 测试编译验证脚本" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# 检查是否在项目根目录
if (-not (Test-Path "CMakeLists.txt")) {
    Write-Host "错误：请在项目根目录运行此脚本" -ForegroundColor Red
    exit 1
}

# 1. 清理旧构建（可选）
Write-Host "[1/5] 清理构建目录..." -ForegroundColor Yellow
if (Test-Path $BuildDir) {
    Write-Host "  移除旧的构建目录: $BuildDir" -ForegroundColor Gray
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Path $BuildDir | Out-Null

# 2. 配置CMake
Write-Host "[2/5] 配置CMake..." -ForegroundColor Yellow
Push-Location $BuildDir
try {
    cmake .. -DCMAKE_BUILD_TYPE=$BuildType -DBUILD_TESTING=ON
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  CMake配置失败！" -ForegroundColor Red
        exit 1
    }
    Write-Host "  ✓ CMake配置成功" -ForegroundColor Green
}
finally {
    Pop-Location
}

# 3. 编译所有测试
Write-Host "[3/5] 编译测试..." -ForegroundColor Yellow
Push-Location $BuildDir
try {
    $cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
    cmake --build . --target all -j $cores
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  编译失败！" -ForegroundColor Red
        exit 1
    }
    Write-Host "  ✓ 编译成功" -ForegroundColor Green
}
finally {
    Pop-Location
}

# 4. 列出所有测试
Write-Host "[4/5] 测试列表..." -ForegroundColor Yellow
Push-Location $BuildDir
try {
    ctest -N | Select-String "Test #" | ForEach-Object {
        Write-Host "  $_" -ForegroundColor Gray
    }
}
finally {
    Pop-Location
}

# 5. 运行测试
Write-Host "[5/5] 运行测试..." -ForegroundColor Yellow
Push-Location $BuildDir
try {
    ctest --output-on-failure --verbose
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  测试运行失败！" -ForegroundColor Red
        exit 1
    }
    Write-Host "  ✓ 所有测试通过" -ForegroundColor Green
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "✓ 验证完成！" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "测试统计：" -ForegroundColor Cyan
Push-Location $BuildDir
ctest -N | Select-String "Total Tests:"
Pop-Location

Write-Host ""
Write-Host "提示：可以使用以下命令单独运行测试" -ForegroundColor Yellow
Write-Host "  cd $BuildDir" -ForegroundColor Gray
Write-Host "  ctest -R spsc_queue_tests -V" -ForegroundColor Gray
Write-Host "  ctest -R stub_consumer_tests -V" -ForegroundColor Gray
