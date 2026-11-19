# build-plugin.ps1 — 手工构建 IDEA 插件 zip（不走 Gradle）。
#
# ── 为什么不用 gradle-intellij-plugin ──────────────────────
#
# 正规路线要下载 Gradle 发行版 + 整个 IntelliJ 平台源码（几百 MB），
# 网络一旦受限就全线卡死。手编路线只需要两样东西，IDEA 自带：
#   1. JBR 里的 javac 17（IDEA 2024.1 要求插件以 Java 17 编译）
#   2. lib\*.jar —— 直接对着本机 IDEA 的平台编译，兼容性天然精确
#
# 产物布局（IDEA 插件 zip 的标准结构）：
#   x-lang-idea-<version>.zip
#   └── lib\
#       └── x-lang-idea.jar     # classes + META-INF/plugin.xml + icons
#
# 用法：powershell -File tools\build-plugin.ps1

param(
    [string]$IdeaDir = "D:\DevTool\IntelliJ IDEA 2024.1.7",
    [string]$Version = "0.3.3"
)

$ErrorActionPreference = "Stop"
$root      = Split-Path -Parent $PSScriptRoot
$srcDir    = Join-Path $root "src"
$resDir    = Join-Path $root "resources"
$outDir    = Join-Path $root "build\classes"
$stageDir  = Join-Path $root "build\stage"
$distDir   = Join-Path $root "dist"

$jbr   = Join-Path $IdeaDir "jbr\bin"
$javac = Join-Path $jbr "javac.exe"
# JBR 只带编译器不带 jar 工具，打包用系统 JDK 的（jar 只是打 zip，版本无所谓）
$jar   = Join-Path $jbr "jar.exe"
if (-not (Test-Path $jar)) {
    $jar = "D:\DevEnv\Java\bin\jar.exe"
}
$cp    = Join-Path $IdeaDir "lib\*"

if (-not (Test-Path $javac)) { throw "找不到 JBR 的 javac：$javac" }
if (-not (Test-Path $jar))   { throw "找不到 jar 工具（JBR 和 D:\DevEnv\Java 都没有）" }

# 1. 编译（源码含中文注释，必须 UTF-8）
if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$sources = Get-ChildItem $srcDir -Recurse -Filter *.java | ForEach-Object { $_.FullName }
& $javac -encoding UTF-8 -source 17 -target 17 -nowarn `
    -cp "$cp" -d "$outDir" @sources
if ($LASTEXITCODE -ne 0) { throw "javac 编译失败（退出码 $LASTEXITCODE）" }

# 2. 打 jar（classes + 资源），plugin.xml 必须在 jar 的 META-INF 里
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stageDir "lib") | Out-Null
$pluginJar = Join-Path $stageDir "lib\x-lang-idea.jar"
& $jar cf "$pluginJar" -C "$outDir" . -C "$resDir" .
if ($LASTEXITCODE -ne 0) { throw "jar 打包失败（退出码 $LASTEXITCODE）" }

# 3. 压 zip
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$zip = Join-Path $distDir "x-lang-idea-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stageDir "lib") -DestinationPath $zip

Write-Host ""
Write-Host "构建完成：$zip"
Write-Host ("大小：{0:N0} 字节" -f (Get-Item $zip).Length)
