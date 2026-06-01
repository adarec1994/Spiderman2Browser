# POST a tools/call to IDA-MCP with arguments read from a JSON file (no CLI quoting).
#   pwsh ida_post.ps1 <tool> <args.json>
param(
  [Parameter(Mandatory=$true)][string]$Tool,
  [Parameter(Mandatory=$true)][string]$ArgsFile
)
$ErrorActionPreference = "Stop"
$u = "http://127.0.0.1:13337/mcp"
$argObj = Get-Content -Raw $ArgsFile | ConvertFrom-Json
$body = @{ jsonrpc="2.0"; id=1; method="tools/call"; params=@{ name=$Tool; arguments=$argObj } } |
        ConvertTo-Json -Depth 30 -Compress
try {
  $r = Invoke-WebRequest -Uri $u -Method POST -Body $body -ContentType "application/json" `
       -Headers @{ "Accept" = "application/json, text/event-stream" } -TimeoutSec 90 -UseBasicParsing
  $obj = $r.Content | ConvertFrom-Json
  if ($null -ne $obj.result.structuredContent) { $obj.result.structuredContent | ConvertTo-Json -Depth 30 }
  elseif ($null -ne $obj.result.content) { ($obj.result.content | ForEach-Object { $_.text }) -join "`n" }
  elseif ($obj.error) { "RPC_ERROR: " + ($obj.error | ConvertTo-Json -Depth 10) }
  else { $obj | ConvertTo-Json -Depth 30 }
} catch {
  $resp = $_.Exception.Response
  if ($resp) { $sr = New-Object System.IO.StreamReader($resp.GetResponseStream()); "HTTP_ERR $($resp.StatusCode.value__): " + $sr.ReadToEnd() }
  else { "ERR: $($_.Exception.Message)" }
}
