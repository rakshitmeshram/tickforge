# Windows equivalent of scripts/reproduce.sh: rebuild, run all tests and
# regenerate every number and figure in the README.
#
#   powershell -ExecutionPolicy Bypass -File scripts\reproduce.ps1
#
# Works with Visual Studio (multi-config) and Ninja/MinGW (single-config)
# generators: tools are placed in build\ either way.
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

function Invoke-Step {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

Write-Host '== build (Release) =='
Invoke-Step cmake @('-S', '.', '-B', 'build', '-DCMAKE_BUILD_TYPE=Release')
Invoke-Step cmake @('--build', 'build', '--config', 'Release', '-j')

Write-Host '== C++ tests =='
Invoke-Step ctest @('--test-dir', 'build', '-C', 'Release', '--output-on-failure')

Write-Host '== Python tests =='
Push-Location python
try { Invoke-Step python @('-m', 'pytest', '-q') } finally { Pop-Location }

New-Item -ItemType Directory -Force data, results | Out-Null
$bench = 'build\tf_bench.exe'
$gen = 'build\tf_gen.exe'
$replay = 'build\tf_replay.exe'

Write-Host '== benchmark: typical book =='
Invoke-Step $bench @('--n', '2000000', '--seed', '1', '--runs', '5', '--json', 'results\bench.json',
                     '--samples', 'results\latency_samples.csv')
Write-Host '== benchmark: deep book =='
Invoke-Step $bench @('--n', '2000000', '--seed', '1', '--runs', '3', '--live', '200000', '--decay_pct', '2',
                     '--json', 'results\bench_deep.json')
Invoke-Step python @('research\plot_latency.py', 'results\latency_samples.csv', 'results\bench.json', 'results')

Write-Host '== order book imbalance study =='
Invoke-Step $gen @('--n', '2000000', '--seed', '7', '--out', 'data\flow.bin')
Invoke-Step $replay @('--in', 'data\flow.bin', '--book', 'data\book.csv', '--trades', 'data\trades.csv')
python research\obi_signal.py data\book.csv results | Tee-Object -FilePath results\obi_stdout.txt
if ($LASTEXITCODE -ne 0) { throw 'obi_signal.py failed' }

Write-Host '== market-making backtest =='
python research\market_maker.py 200000 10 results | Tee-Object -FilePath results\mm_stdout.txt
if ($LASTEXITCODE -ne 0) { throw 'market_maker.py failed' }
