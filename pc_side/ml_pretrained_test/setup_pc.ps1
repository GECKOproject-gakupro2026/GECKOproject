[CmdletBinding()]
param(
    [string]$WorkspaceRoot = "",
    [switch]$InstallModelZooDependencies
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
    $WorkspaceRoot = Split-Path -Parent $RepoRoot
}
$WorkspaceRoot = [System.IO.Path]::GetFullPath($WorkspaceRoot)

$ModelZoo = Join-Path $WorkspaceRoot "stm32ai-modelzoo"
$Services = Join-Path $WorkspaceRoot "stm32ai-modelzoo-services"
$ModelZooCommit = "1423c78953a830903485135febe1dd98ff31aed8"
$ServicesCommit = "0f6210ed5156126b782e1c43249063a477484b20"
$ModelRelativePath = "audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_int8.tflite"
$ModelPath = Join-Path $ModelZoo $ModelRelativePath
$Verifier = Join-Path $PSScriptRoot "verify_model.py"
$ResultDir = Join-Path $RepoRoot "test_results/ml_pretrained_smoke/pc_setup"

function Assert-Command {
    param([Parameter(Mandatory = $true)][string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command is unavailable: $Name"
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Program,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $Program $($Arguments -join ' ')"
    }
}

function Sync-PinnedRepository {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Commit
    )

    if (Test-Path (Join-Path $Path ".git")) {
        $origin = (& git -C $Path remote get-url origin).Trim()
        if ($LASTEXITCODE -ne 0 -or $origin -ne $Url) {
            throw "Unexpected Git origin at ${Path}: $origin"
        }
        $dirty = & git -C $Path status --porcelain
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to inspect repository: $Path"
        }
        if ($dirty) {
            throw "Repository has uncommitted changes; refusing checkout: $Path"
        }
        Invoke-Checked git -C $Path fetch --depth 1 origin $Commit
    }
    elseif (Test-Path $Path) {
        throw "Path exists but is not a Git repository: $Path"
    }
    else {
        Invoke-Checked git clone --no-checkout $Url $Path
    }

    Invoke-Checked git -C $Path checkout --detach $Commit
    $actual = (& git -C $Path rev-parse HEAD).Trim()
    if ($actual -ne $Commit) {
        throw "Commit mismatch at ${Path}: expected $Commit, got $actual"
    }
}

Assert-Command git
Assert-Command python
Invoke-Checked git lfs version

$pythonVersion = & python -c "import sys; print('.'.join(map(str, sys.version_info[:3])))"
if ($LASTEXITCODE -ne 0) {
    throw "Unable to determine Python version"
}
$pythonMajorMinor = & python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"
if ($pythonMajorMinor.Trim() -ne "3.12") {
    throw "Python 3.12 is required by the pinned Model Zoo Services release; found $pythonVersion"
}

New-Item -ItemType Directory -Force -Path $WorkspaceRoot | Out-Null
$previousLfsSkipSmudge = $env:GIT_LFS_SKIP_SMUDGE
$env:GIT_LFS_SKIP_SMUDGE = "1"
try {
    Sync-PinnedRepository $ModelZoo "https://github.com/STMicroelectronics/stm32ai-modelzoo.git" $ModelZooCommit
}
finally {
    if ($null -eq $previousLfsSkipSmudge) {
        Remove-Item Env:GIT_LFS_SKIP_SMUDGE -ErrorAction SilentlyContinue
    }
    else {
        $env:GIT_LFS_SKIP_SMUDGE = $previousLfsSkipSmudge
    }
}
Sync-PinnedRepository $Services "https://github.com/STMicroelectronics/stm32ai-modelzoo-services.git" $ServicesCommit

Invoke-Checked git -C $ModelZoo lfs pull --include=$ModelRelativePath
Invoke-Checked git -C $Services submodule update --init --recursive

New-Item -ItemType Directory -Force -Path $ResultDir | Out-Null
$IntegrityJson = Join-Path $ResultDir "model_integrity.json"
Invoke-Checked python $Verifier $ModelPath --json-out $IntegrityJson

$sourceVersions = @(
    "target_repository=$((& git -C $RepoRoot rev-parse HEAD).Trim())"
    "model_zoo=$((& git -C $ModelZoo rev-parse HEAD).Trim())"
    "model_zoo_services=$((& git -C $Services rev-parse HEAD).Trim())"
    "python=$($pythonVersion.Trim())"
    "model=$ModelPath"
)
$sourceVersions | Set-Content -Encoding UTF8 (Join-Path $ResultDir "source_versions.txt")

if ($InstallModelZooDependencies) {
    $Venv = Join-Path $WorkspaceRoot ".venv-stm32ai-modelzoo"
    if (-not (Test-Path $Venv)) {
        Invoke-Checked python -m venv $Venv
    }
    $VenvPython = Join-Path $Venv "Scripts/python.exe"
    Invoke-Checked $VenvPython -m pip install --upgrade pip
    Invoke-Checked $VenvPython -m pip install -r (Join-Path $Services "requirements.txt")
    Invoke-Checked $VenvPython $Verifier $ModelPath --inspect-io --json-out (Join-Path $ResultDir "model_io.json")
    Invoke-Checked $VenvPython -m pip freeze
}

Write-Host "PC preparation completed."
Write-Host "Model: $ModelPath"
Write-Host "Results: $ResultDir"
Write-Host "Services: $Services"
