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
$gitConfigIndex = if ([string]::IsNullOrWhiteSpace($env:GIT_CONFIG_COUNT)) { 0 } else { [int]$env:GIT_CONFIG_COUNT }
[Environment]::SetEnvironmentVariable("GIT_CONFIG_KEY_$gitConfigIndex", "safe.directory", "Process")
[Environment]::SetEnvironmentVariable("GIT_CONFIG_VALUE_$gitConfigIndex", $ModelZoo, "Process")
[Environment]::SetEnvironmentVariable("GIT_CONFIG_KEY_$($gitConfigIndex + 1)", "safe.directory", "Process")
[Environment]::SetEnvironmentVariable("GIT_CONFIG_VALUE_$($gitConfigIndex + 1)", $Services, "Process")
$env:GIT_CONFIG_COUNT = [string]($gitConfigIndex + 2)
$ModelZooCommit = "1423c78953a830903485135febe1dd98ff31aed8"
$ServicesCommit = "0f6210ed5156126b782e1c43249063a477484b20"
$ModelRelativePath = "audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_int8.tflite"
$ModelPath = Join-Path $ModelZoo $ModelRelativePath
$ModelSha256 = "cd75689f072fac00d2a0fca063faec0ae0070a78d128d7f8d60c0f7ff88cd48d"
$ModelSize = 184240
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
        [Parameter(Mandatory = $true)][string]$Commit,
        [string[]]$SparsePaths = @()
    )

    if (Test-Path (Join-Path $Path ".git")) {
        $origin = (& git -C $Path remote get-url origin).Trim()
        if ($LASTEXITCODE -ne 0 -or $origin -ne $Url) {
            throw "Unexpected Git origin at ${Path}: $origin"
        }
        & git -C $Path status --porcelain | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to inspect repository: $Path"
        }
        & git -C $Path diff --quiet --
        $unstagedDiff = $LASTEXITCODE
        & git -C $Path diff --cached --quiet --
        $stagedDiff = $LASTEXITCODE
        $untracked = & git -C $Path ls-files --others --exclude-standard
        if ($LASTEXITCODE -ne 0 -or $unstagedDiff -gt 1 -or $stagedDiff -gt 1) {
            throw "Unable to inspect repository differences: $Path"
        }
        if ($unstagedDiff -eq 1 -or $stagedDiff -eq 1 -or $untracked) {
            throw "Repository has uncommitted changes; refusing checkout: $Path"
        }
        Invoke-Checked git -C $Path fetch --depth 1 --filter=blob:none origin $Commit
    }
    elseif (Test-Path $Path) {
        throw "Path exists but is not a Git repository: $Path"
    }
    else {
        Invoke-Checked git init $Path
        Invoke-Checked git -C $Path remote add origin $Url
        if ($SparsePaths.Count -gt 0) {
            Invoke-Checked git -C $Path sparse-checkout init --no-cone
            $sparseArguments = @("-C", $Path, "sparse-checkout", "set", "--no-cone") + $SparsePaths
            Invoke-Checked git @sparseArguments
        }
        Invoke-Checked git -C $Path fetch --depth 1 --filter=blob:none origin $Commit
    }

    Invoke-Checked git -C $Path checkout --detach FETCH_HEAD
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
    Sync-PinnedRepository $ModelZoo "https://github.com/STMicroelectronics/stm32ai-modelzoo.git" $ModelZooCommit -SparsePaths @("/.gitattributes", "/$ModelRelativePath")
}
finally {
    if ($null -eq $previousLfsSkipSmudge) {
        Remove-Item Env:GIT_LFS_SKIP_SMUDGE -ErrorAction SilentlyContinue
    }
    else {
        $env:GIT_LFS_SKIP_SMUDGE = $previousLfsSkipSmudge
    }
}

$modelIsValid = $false
if (Test-Path $ModelPath) {
    $existingModel = Get-Item $ModelPath
    $existingHash = (Get-FileHash -Algorithm SHA256 $ModelPath).Hash.ToLowerInvariant()
    $modelIsValid = $existingModel.Length -eq $ModelSize -and $existingHash -eq $ModelSha256
}
if (-not $modelIsValid) {
    $downloadedModel = "$ModelPath.download"
    $smudge = Start-Process -FilePath "git-lfs.exe" `
        -ArgumentList @("smudge", "--", $ModelRelativePath) `
        -WorkingDirectory $ModelZoo `
        -RedirectStandardInput $ModelPath `
        -RedirectStandardOutput $downloadedModel `
        -NoNewWindow -Wait -PassThru
    if ($smudge.ExitCode -ne 0) {
        throw "Git LFS smudge failed ($($smudge.ExitCode)): $ModelRelativePath"
    }
    $downloadedFile = Get-Item $downloadedModel
    $downloadedHash = (Get-FileHash -Algorithm SHA256 $downloadedModel).Hash.ToLowerInvariant()
    if ($downloadedFile.Length -ne $ModelSize -or $downloadedHash -ne $ModelSha256) {
        throw "Downloaded model integrity mismatch: $downloadedModel"
    }
    Move-Item -LiteralPath $downloadedModel -Destination $ModelPath -Force
}

New-Item -ItemType Directory -Force -Path $ResultDir | Out-Null
$IntegrityJson = Join-Path $ResultDir "model_integrity.json"
Invoke-Checked python $Verifier $ModelPath --json-out $IntegrityJson

$servicesVersion = "$ServicesCommit (not downloaded)"
if ($InstallModelZooDependencies) {
    Sync-PinnedRepository $Services "https://github.com/STMicroelectronics/stm32ai-modelzoo-services.git" $ServicesCommit
    Invoke-Checked git -C $Services submodule update --init --recursive
    $servicesVersion = (& git -C $Services rev-parse HEAD).Trim()
}

$sourceVersions = @(
    "target_repository=$((& git -C $RepoRoot rev-parse HEAD).Trim())"
    "model_zoo=$((& git -C $ModelZoo rev-parse HEAD).Trim())"
    "model_zoo_services=$servicesVersion"
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
