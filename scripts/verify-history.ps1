$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $record = Get-Content -Raw -LiteralPath 'docs/history/checkpoints.json' | ConvertFrom-Json
    if ($record.schema -ne 1 -or $record.baseline -cnotmatch '^[0-9a-f]{40}$' -or
        $record.history_tip -cnotmatch '^[0-9a-f]{40}$') { throw 'Invalid history manifest' }
    $commits = @(git rev-list --reverse $record.history_tip)
    if ($LASTEXITCODE -ne 0 -or $commits.Count -ne $record.checkpoints.Count + 1) {
        throw 'Fetch complete history before running the audit'
    }
    $before = git rev-parse "$($record.baseline)^{tree}"
    if ($LASTEXITCODE -ne 0) { throw 'Missing public baseline' }
    $after = git rev-parse "$($record.history_tip)^{tree}"
    if ($LASTEXITCODE -ne 0 -or $before -cne $after) { throw 'Completed history differs from public baseline' }
    $tag = git rev-parse ("refs/tags/" + $record.original_release_tag)
    if ($LASTEXITCODE -ne 0 -or $tag -cne $record.original_release_tag_object) { throw 'Original release tag changed' }
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $blobs = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $contentPattern = 'BEGIN (OPENSSH|RSA|EC|DSA|ENCRYPTED) PRIVATE KEY|ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{70,}|[A-Z]:[/\\]Users[/\\]|[\w.+-]+@(gmail|outlook)\.com'
    for ($i=0; $i -lt $commits.Count; $i++) {
        $commit = $commits[$i]
        $parents = (git show -s --format=%P $commit).Trim()
        if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect history parents' }
        if (($i -eq 0 -and $parents) -or ($i -gt 0 -and $parents -cne $commits[$i-1])) {
            throw 'Reconstruction must have one root and linear ancestry'
        }
        $changed = @(git diff-tree --root --no-commit-id --name-only -r $commit)
        if ($LASTEXITCODE -ne 0 -or -not $changed.Count) { throw 'Empty history step' }
        $message = (git show -s --format=%B $commit) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $message -match $contentPattern) { throw 'Unexpected history message content' }
        if ($i -lt $record.checkpoints.Count) {
            $step = $record.checkpoints[$i]
            if ($step.commit -cne $commit -or $step.source_checkpoint -cnotmatch '^[0-9a-f]{40}$' -or
                $step.group -cnotin @('implementation','conformance') -or
                $step.files_changed -ne $changed.Count -or
                -not $message.Contains("Source-checkpoint: $($step.source_checkpoint)") -or
                -not $message.Contains("Reconstruction-group: $($step.group)")) {
                throw 'History step does not match its provenance'
            }
        }
        foreach ($line in @(git -c core.quotepath=false ls-tree -r $commit)) {
            if ($line -cnotmatch '^(100644|100755) blob ([0-9a-f]{40})\t(.+)$') { throw 'Unexpected history entry' }
            [void]$blobs.Add($Matches[2]); [void]$paths.Add($Matches[3])
        }
        if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect history tree' }
    }
    foreach ($path in $paths) {
        $roots = @('.editorconfig','.gitattributes','.gitignore','README.md','CONTRIBUTING.md',
            'CHANGELOG.md','build.bat')
        $directories = @('.github','benchmarks','docs','examples','experiments','include',
            'runtime','scripts','src','stdlib','tests','tools')
        if (($path -notmatch '/' -and $path -cnotin $roots) -or
            ($path -match '/' -and ($path -split '/',2)[0] -cnotin $directories) -or
            ($path -match '/' -and
             (($path -replace '^\.github/','') -replace '(^|/)\.gitignore$','$1ignore') -match '(^|/)\.') -or
            $path -match '^(build|dist|output|tmp|submission|docs/(proposal|verification))/' -or
            $path -match '\.(pdf|docx|zip|exe|dll|pdb|pem|key)$') { throw "Unexpected historical path: $path" }
    }
    foreach ($blob in $blobs) {
        $content = (git cat-file blob $blob) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $content -match $contentPattern) { throw "Unexpected content in historical blob: $blob" }
    }
    Write-Output "History verified: $($commits.Count) nonempty commits, $($paths.Count) paths, $($blobs.Count) blobs; original release and completed tree preserved"
} finally { Pop-Location }
