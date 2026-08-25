$ErrorActionPreference = "Continue"

$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $AppDir "Output"
$FirmwareDir = [System.IO.Path]::GetFullPath((Join-Path $AppDir "..\..\server\firmware"))
$FromElf = "C:\Keil_v5\ARM\ARMCC\Bin\fromelf.exe"
$ArmLink = "C:\Keil_v5\ARM\ARMCC\Bin\armlink.exe"
$Log = Join-Path $AppDir "post_build_bin.log"

$AxfA = Join-Path $OutDir "app.axf"
$LnpA = Join-Path $OutDir "app.lnp"
$AxfB = Join-Path $OutDir "app_b.axf"
$LnpB = Join-Path $OutDir "app_b.lnp"
$SctB = Join-Path $AppDir "STM32F103ZETX_APP_B.sct"
$BinA = Join-Path $FirmwareDir "stm32_app_a.bin"
$BinB = Join-Path $FirmwareDir "stm32_app_b.bin"
$BinCompat = Join-Path $FirmwareDir "stm32_app.bin"

function Write-Log($Message) {
    Add-Content -Path $Log -Value $Message -Encoding UTF8
}

function Run-Tool($File, [string[]]$Arguments) {
    Write-Log ("> " + $File + " " + ($Arguments -join " "))
    $stdout = [System.IO.Path]::GetTempFileName()
    $stderr = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $File -ArgumentList $Arguments -Wait -PassThru -NoNewWindow -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if ((Get-Item $stdout).Length -gt 0) { Get-Content $stdout | Add-Content -Path $Log -Encoding UTF8 }
        if ((Get-Item $stderr).Length -gt 0) { Get-Content $stderr | Add-Content -Path $Log -Encoding UTF8 }
        if ($process.ExitCode -ne 0) {
            throw "$File failed with exit code $($process.ExitCode)"
        }
    } finally {
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    }
}

Set-Content -Path $Log -Value "post_build start" -Encoding UTF8
New-Item -ItemType Directory -Force -Path $FirmwareDir | Out-Null

if (!(Test-Path -LiteralPath $AxfA)) { throw "AXF not found: $AxfA" }
if (!(Test-Path -LiteralPath $LnpA)) { throw "LNP not found: $LnpA" }
if (!(Test-Path -LiteralPath $SctB)) { throw "B scatter not found: $SctB" }

Remove-Item -LiteralPath $BinA, $BinB, $BinCompat -Force -ErrorAction SilentlyContinue
Run-Tool $FromElf @("--bin", "--output=$BinA", $AxfA)

$lnpText = Get-Content -LiteralPath $LnpA -Raw
$lnpText = $lnpText.Replace('--scatter ".\Output\app.sct"', '--scatter "' + $SctB + '"')
$lnpText = $lnpText.Replace('-o .\Output\app.axf', '-o ' + $AxfB)
Set-Content -Path $LnpB -Value $lnpText -Encoding ASCII

Push-Location $AppDir
try {
    Run-Tool $ArmLink @("--via", $LnpB)
} finally {
    Pop-Location
}
Run-Tool $FromElf @("--bin", "--output=$BinB", $AxfB)
Copy-Item -LiteralPath $BinB -Destination $BinCompat -Force

if (!(Test-Path -LiteralPath $BinA)) { throw "A BIN not created: $BinA" }
if (!(Test-Path -LiteralPath $BinB)) { throw "B BIN not created: $BinB" }
if (!(Test-Path -LiteralPath $BinCompat)) { throw "compat BIN not created: $BinCompat" }

Write-Log "A BIN: $BinA"
Write-Log "B BIN: $BinB"
Write-Log "Compat BIN: $BinCompat"
Write-Log "post_build success"
