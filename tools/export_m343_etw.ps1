param(
    [Parameter(Mandatory=$true)][string]$Trace,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$RawExporter
)
$ErrorActionPreference = 'Stop'
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory must be new' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$resolved = (Resolve-Path -LiteralPath $Trace).Path
& $RawExporter $resolved (Join-Path $output 'raw.csv') | Set-Content -LiteralPath (Join-Path $output 'raw-status.json') -Encoding UTF8
if ($LASTEXITCODE -ne 0) { throw 'Raw ETL export failed; retain partial evidence' }
Add-Type -ReferencedAssemblies @([System.Diagnostics.Eventing.Reader.EventLogQuery].Assembly.Location,'System.Collections.dll') -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics.Eventing.Reader;
using System.IO;
public static class M343XmlExport {
    public static long Export(string path, string directory) {
        var dxg = new Guid("802ec45a-1e99-4b83-9920-87c98277ba9d");
        var d3d = new Guid("5d8087dd-3a9b-4f56-90df-49196cdc4f11");
        var dxgi = new Guid("ca11c036-0102-4a2d-a6ad-f03cfed5d3c9");
        var cpu = new Guid("3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c");
        long count = 0;
        using (var gpu = new StreamWriter(Path.Combine(directory,"gpu.xml")))
        using (var system = new StreamWriter(Path.Combine(directory,"system.xml")))
        using (var reader = new EventLogReader(new EventLogQuery(path,PathType.FilePath))) {
            reader.BatchSize = 64;
            gpu.WriteLine("<events>"); system.WriteLine("<events>");
            EventRecord entry;
            while ((entry = reader.ReadEvent()) != null) using (entry) {
                if (entry.ProviderId == dxg || entry.ProviderId == d3d) { gpu.WriteLine(entry.ToXml()); ++count; }
                else if (entry.ProviderId == dxgi || entry.ProviderId == cpu) { system.WriteLine(entry.ToXml()); ++count; }
            }
            gpu.WriteLine("</events>"); system.WriteLine("</events>");
        }
        return count;
    }
}
'@
$count = [M343XmlExport]::Export($resolved,$output)
[ordered]@{trace=$resolved; sha256=(Get-FileHash -LiteralPath $resolved).Hash; xml_events=$count; execution_attribution_validated=$false} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'export-status.json') -Encoding UTF8
Write-Output "Exported $count GPU/system events; raw/XML identity and clock validation still required"
