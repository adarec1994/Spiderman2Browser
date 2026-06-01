# Direct IDA-Pro-MCP caller over HTTP JSON-RPC (bypasses the disconnected MCP client).
# Args are passed as key=value pairs to avoid CLI JSON-quote mangling.
#   pwsh ida.ps1 server_health
#   pwsh ida.ps1 search_text pattern=nal_quaternion
#   pwsh ida.ps1 decompile address=0x354560
#   pwsh ida.ps1 get_bytes address=0x1b0000 size=64
# Numeric-looking values stay strings (IDA accepts hex strings for addresses).
# Use raw JSON instead with:  pwsh ida.ps1 <tool> --json '{"k":"v"}'
param(
  [Parameter(Mandatory=$true)][string]$Tool,
  [Parameter(ValueFromRemainingArguments=$true)][string[]]$Rest
)
$ErrorActionPreference = "Stop"
$u = "http://127.0.0.1:13337/mcp"

$argObj = @{}
if ($Rest -and $Rest.Count -ge 1 -and $Rest[0] -eq "--json") {
  $argObj = ($Rest[1..($Rest.Count-1)] -join " ") | ConvertFrom-Json
} elseif ($Rest) {
  foreach ($kv in $Rest) {
    $i = $kv.IndexOf("=")
    if ($i -lt 0) { continue }
    $k = $kv.Substring(0,$i)
    $v = $kv.Substring($i+1)
    $argObj[$k] = $v
  }
}

$body = @{
  jsonrpc = "2.0"; id = 1; method = "tools/call"
  params  = @{ name = $Tool; arguments = $argObj }
} | ConvertTo-Json -Depth 20 -Compress

try {
  $r = Invoke-WebRequest -Uri $u -Method POST -Body $body -ContentType "application/json" `
       -Headers @{ "Accept" = "application/json, text/event-stream" } -TimeoutSec 90 -UseBasicParsing
  $obj = $r.Content | ConvertFrom-Json
  if ($null -ne $obj.result.structuredContent) {
    $obj.result.structuredContent | ConvertTo-Json -Depth 30
  } elseif ($null -ne $obj.result.content) {
    ($obj.result.content | ForEach-Object { $_.text }) -join "`n"
  } elseif ($obj.error) {
    "RPC_ERROR: " + ($obj.error | ConvertTo-Json -Depth 10)
  } else {
    $obj | ConvertTo-Json -Depth 30
  }
} catch {
  $resp = $_.Exception.Response
  if ($resp) {
    $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
    "HTTP_ERR $($resp.StatusCode.value__): " + $sr.ReadToEnd()
  } else { "ERR: $($_.Exception.Message)" }
}
