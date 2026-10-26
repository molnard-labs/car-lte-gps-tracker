param(
    [string]$ToolchainBin = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $ToolchainBin) { $env:PATH = "$ToolchainBin;$env:PATH" }
foreach ($name in @('gcc', 'mingw32-make', 'cppcheck', 'node', 'python')) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) { throw "Required tool is not available: $name" }
}

$repoRoot = Join-Path $PSScriptRoot '..'
Push-Location $repoRoot
try {
    Write-Host 'Running portable tracker host suites'
    & mingw32-make -C firmware/test
    if ($LASTEXITCODE -ne 0) { throw 'Host tests failed' }

    Write-Host 'Checking portable tracker C sources'
    $checkArguments = @(
        '--enable=warning,performance,portability',
        '--suppress=missingIncludeSystem', '--suppress=missingInclude',
        '--suppress=unusedFunction', '--suppress=unusedStructMember', '--suppress=checkersReport',
        '--error-exitcode=1', '--std=c11', '-U', 'ESP_PLATFORM',
        '-I', 'firmware/components/tracker_core/include',
        '-I', 'firmware/components/tracker_store/include',
        '-I', 'firmware/components/tracker_setup', '-I', 'firmware/main',
        'firmware/components/tracker_core/tracker_core.c',
        'firmware/components/tracker_store/tracker_store.c',
        'firmware/components/tracker_setup/setup_policy.c',
        'firmware/main/tracker_policy.c'
    )
    & cppcheck @checkArguments
    if ($LASTEXITCODE -ne 0) { throw 'Portable C static analysis failed' }

    Write-Host 'Checking embedded setup JavaScript and provisioning Python syntax'
    $html = Get-Content -LiteralPath firmware/components/tracker_setup/setup.html -Raw -Encoding utf8
    $javascript = [regex]::Match($html, '(?s)<script>(.*?)</script>').Groups[1].Value
    if (-not $javascript) { throw 'Setup page script was not found' }
    $javascript | node --check
    if ($LASTEXITCODE -ne 0) { throw 'Setup JavaScript syntax check failed' }
    & node firmware/components/tracker_setup/test/test_setup_ui.cjs
    if ($LASTEXITCODE -ne 0) { throw 'Setup UI behavior tests failed' }
    & python -m py_compile tools/provision_tracker.py
    if ($LASTEXITCODE -ne 0) { throw 'Provisioning Python syntax check failed' }
    & python tools/test_provision_tracker.py
    if ($LASTEXITCODE -ne 0) { throw 'Provisioning workflow tests failed' }
    & python -m py_compile tools/configure_tracker_access.py
    if ($LASTEXITCODE -ne 0) { throw 'Tracker Access provisioning syntax check failed' }
    & python tools/test_configure_tracker_access.py
    if ($LASTEXITCODE -ne 0) { throw 'Tracker Access provisioning tests failed' }
} finally {
    Pop-Location
}
Write-Host 'Local host checks passed. Run the ESP-IDF v5.5.6 firmware build separately; these checks do not validate hardware.'
