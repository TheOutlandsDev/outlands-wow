[CmdletBinding()]
param(
    [string]$MySqlRoot = 'C:\Program Files\MySQL\MySQL Server 8.4',
    [string]$CredentialsPath = 'C:\wow-server\config\credentials.json',
    [string]$OutputRoot,
    [string]$Release = 'la25-social-conversation-v8',
    [string[]]$Schemas = @(
        'la_ahfinal11_auth',
        'la_ahfinal11_characters',
        'la_ahfinal11_world',
        'la_ahfinal11_playerbots'
    )
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)
$startedUtc = [DateTime]::UtcNow

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputRoot = "C:\wow-server\backups\database\gate2-la25-logical-$stamp"
}
if (Test-Path -LiteralPath $OutputRoot) {
    throw "Refusing to overwrite export directory: $OutputRoot"
}
if ($Schemas.Count -ne 4 -or ($Schemas | Select-Object -Unique).Count -ne 4) {
    throw 'Exactly four unique auth, characters, world, and playerbots schemas are required.'
}
foreach ($schema in $Schemas) {
    if ($schema -notmatch '^[A-Za-z0-9_]+$') {
        throw "Unsafe schema name: $schema"
    }
}

$mysql = Join-Path $MySqlRoot 'bin\mysql.exe'
$mysqldump = Join-Path $MySqlRoot 'bin\mysqldump.exe'
foreach ($required in @($CredentialsPath, $mysql, $mysqldump)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required export input is missing: $required"
    }
}

$gameProcesses = @(Get-Process -Name worldserver,authserver,graceful_process_host -ErrorAction SilentlyContinue)
$protectedListeners = @(
    Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
        Where-Object { $_.LocalPort -in 3724, 8085, 8888, 18086 }
)
if ($gameProcesses.Count -ne 0 -or $protectedListeners.Count -ne 0) {
    throw 'Gate 2 export requires terminal zero: stop the game through its lifecycle authority first.'
}
$mysqlListeners = @(Get-NetTCPConnection -State Listen -LocalPort 3306 -ErrorAction SilentlyContinue)
if ($mysqlListeners.Count -ne 1 -or $mysqlListeners[0].LocalAddress -ne '127.0.0.1') {
    throw 'Expected exactly one MySQL listener bound to 127.0.0.1:3306.'
}

$credentials = Get-Content -LiteralPath $CredentialsPath -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace([string]$credentials.acore_password)) {
    throw 'The local credentials file does not contain acore_password.'
}
$tempClient = Join-Path $env:TEMP ("wow-gate2-{0}.ini" -f [Guid]::NewGuid().ToString('N'))
$clientText = "[client]`r`nprotocol=tcp`r`nhost=127.0.0.1`r`nport=3306`r`nuser=acore`r`npassword=$($credentials.acore_password)`r`n"

try {
    [IO.File]::WriteAllText($tempClient, $clientText, $utf8)
    & icacls.exe $tempClient '/inheritance:r' '/grant:r' "$($env:USERNAME):(F)" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not restrict the temporary MySQL client file ACL.' }

    $quotedSchemas = ($Schemas | ForEach-Object { "'" + $_.Replace("'", "''") + "'" }) -join ','
    $engineSql = "SELECT ENGINE,COUNT(*) FROM information_schema.tables WHERE table_schema IN ($quotedSchemas) AND table_type='BASE TABLE' GROUP BY ENGINE ORDER BY ENGINE;"
    $engineRows = @(& $mysql "--defaults-extra-file=$tempClient" --batch --raw --skip-column-names "--execute=$engineSql" 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'Could not validate storage engines before export.' }
    $unsupportedEngines = @($engineRows | Where-Object { $_ -notmatch '^(InnoDB|MyISAM)\t' })
    if ($unsupportedEngines.Count -ne 0) {
        throw "Consistent export refused because unsupported storage engines exist: $($unsupportedEngines -join '; ')"
    }

    $onlineSql = "SELECT COUNT(*) FROM $($Schemas[1]).characters WHERE online<>0;"
    $onlineRows = @(& $mysql "--defaults-extra-file=$tempClient" --batch --raw --skip-column-names "--execute=$onlineSql" 2>&1)
    if ($LASTEXITCODE -ne 0 -or $onlineRows.Count -ne 1 -or [int]$onlineRows[0] -ne 0) {
        throw 'Gate 2 export requires zero online characters.'
    }

    New-Item -ItemType Directory -Path $OutputRoot | Out-Null
    & icacls.exe $OutputRoot '/inheritance:r' '/grant:r' "$($env:USERNAME):(OI)(CI)(F)" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Could not restrict the export directory ACL.' }

    $dumpPath = Join-Path $OutputRoot 'la25-four-databases.sql'
    $stderrPath = Join-Path $OutputRoot 'mysqldump.stderr.log'
    $arguments = @(
        "--defaults-extra-file=$tempClient",
        '--lock-all-tables',
        '--quick',
        '--routines',
        '--events',
        '--triggers',
        '--hex-blob',
        '--no-tablespaces',
        '--set-gtid-purged=OFF',
        '--default-character-set=utf8mb4',
        '--databases'
    ) + $Schemas + @("--result-file=$dumpPath")
    $process = Start-Process -FilePath $mysqldump -ArgumentList $arguments -WindowStyle Hidden -RedirectStandardError $stderrPath -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "mysqldump failed with exit code $($process.ExitCode); inspect the restricted stderr log."
    }
    $dump = Get-Item -LiteralPath $dumpPath
    if ($dump.Length -le 0) { throw 'mysqldump produced an empty logical export.' }
    $tail = Get-Content -LiteralPath $dumpPath -Tail 20
    if (-not ($tail -match '^-- Dump completed on ')) {
        throw 'The logical export completion marker is missing.'
    }

    $tableSql = "SELECT table_schema,COUNT(*) FROM information_schema.tables WHERE table_schema IN ($quotedSchemas) GROUP BY table_schema ORDER BY table_schema;"
    $tableRows = @(& $mysql "--defaults-extra-file=$tempClient" --batch --raw --skip-column-names "--execute=$tableSql" 2>&1)
    if ($LASTEXITCODE -ne 0 -or $tableRows.Count -ne 4) { throw 'Could not capture four-schema table counts.' }

    $metricsSql = @"
SELECT 'schema_count',COUNT(*) FROM information_schema.schemata WHERE schema_name IN ($quotedSchemas)
UNION ALL SELECT 'accounts',COUNT(*) FROM $($Schemas[0]).account
UNION ALL SELECT 'characters',COUNT(*) FROM $($Schemas[1]).characters
UNION ALL SELECT 'online_characters',COUNT(*) FROM $($Schemas[1]).characters WHERE online<>0
UNION ALL SELECT 'auctions',COUNT(*) FROM $($Schemas[1]).auctionhouse
UNION ALL SELECT 'world_tables',COUNT(*) FROM information_schema.tables WHERE table_schema='$($Schemas[2])'
UNION ALL SELECT 'playerbots_tables',COUNT(*) FROM information_schema.tables WHERE table_schema='$($Schemas[3])';
"@
    $metricRows = @(& $mysql "--defaults-extra-file=$tempClient" --batch --raw --skip-column-names "--execute=$metricsSql" 2>&1)
    if ($LASTEXITCODE -ne 0 -or $metricRows.Count -ne 7) { throw 'Could not capture required aggregate metrics.' }
    $metrics = [ordered]@{}
    foreach ($row in $metricRows) {
        $parts = ([string]$row) -split "`t", 2
        if ($parts.Count -ne 2) { throw "Invalid aggregate metric row: $row" }
        $metrics[$parts[0]] = [int64]$parts[1]
    }

    $manifest = [ordered]@{
        result = 'PASS'
        change_id = 'database-migration-gate2-export'
        release = $Release
        started_utc = $startedUtc.ToString('o')
        completed_utc = [DateTime]::UtcNow.ToString('o')
        dump_file = $dump.Name
        bytes = $dump.Length
        sha256 = (Get-FileHash -LiteralPath $dumpPath -Algorithm SHA256).Hash.ToLowerInvariant()
        schemas = $Schemas
        online_characters = 0
        storage_engines = $engineRows
        table_counts = $tableRows
        metrics = $metrics
        export_flags = @('lock-all-tables','quick','routines','events','triggers','hex-blob','no-tablespaces','set-gtid-purged=OFF','utf8mb4')
        credentials_recorded = $false
    }
    [IO.File]::WriteAllText(
        (Join-Path $OutputRoot 'manifest.json'),
        ($manifest | ConvertTo-Json -Depth 6) + "`n",
        $utf8
    )
    $manifest | ConvertTo-Json -Depth 6
}
finally {
    $credentials = $null
    $clientText = $null
    if (Test-Path -LiteralPath $tempClient) {
        Remove-Item -LiteralPath $tempClient -Force
    }
}
