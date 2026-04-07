# run_performance_test.ps1
$testDataRoot = Join-Path (Get-Location) "perf_test_data"
$chatsDir = Join-Path $testDataRoot "uam_chats"
$count = 500

Write-Host "--- UAM Performance Test Setup ---" -ForegroundColor Cyan

# 1. Prepare clean test environment
if (Test-Path $testDataRoot) {
    Write-Host "Cleaning existing test data..." -ForegroundColor Gray
    Remove-Item -Path $testDataRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $chatsDir -Force | Out-Null

# 2. Generate test chats
Write-Host "Generating $count performance test chats in $chatsDir..." -ForegroundColor Yellow
$timestamp = "2026-04-07 12:00:00"

for ($i = 1; $i -le $count; $i++) {
    $id = "perf-test-$($i.ToString('000'))"
    $title = "Performance Test Chat #$i"
    
    $json = @"
{
  "id": "$id",
  "provider_id": "gemini-cli",
  "title": "$title",
  "created_at": "$timestamp",
  "updated_at": "$timestamp",
  "rag_enabled": true,
  "prompt_profile_bootstrapped": false,
  "branch_from_message_index": -1,
  "messages": [
    {
      "role": "user",
      "content": "Performance test message for chat $i.",
      "created_at": "$timestamp"
    },
    {
      "role": "assistant",
      "content": "Acknowledged. System stress test in progress for chat $i.",
      "created_at": "$timestamp"
    }
  ]
}
"@
    $json | Out-File -FilePath "$chatsDir\$id.json" -Encoding utf8
}

Write-Host "Successfully generated $count chats." -ForegroundColor Green
Write-Host ""

# 2.5 Launch the application with UAM_DATA_DIR override
$exePath = Join-Path (Get-Location) "Builds\Release\universal_agent_manager.exe"
if (Test-Path $exePath) {
    Write-Host "Launching UAM with test data override..." -ForegroundColor Cyan
    # Set environment variable for the session (inherited by child processes)
    $env:UAM_DATA_DIR = $testDataRoot
    Start-Process -FilePath $exePath
} else {
    Write-Host "Warning: Application executable not found at $exePath." -ForegroundColor Red
    Write-Host "Please launch the app manually with UAM_DATA_DIR environment variable set to: $testDataRoot"
}

Write-Host ""
Write-Host ">>> ACTION REQUIRED <<<" -ForegroundColor White -BackgroundColor Red
Write-Host "1. Keep this terminal open while testing."
Write-Host "2. When finished testing and the app is closed, come back here."
Write-Host "3. Press ENTER to cleanup the test environment."
Write-Host ""

Read-Host "Press ENTER to cleanup test data..."

# 3. Cleanup
Write-Host "Cleaning up test environment..." -ForegroundColor Yellow
if (Test-Path $testDataRoot) {
    Remove-Item -Path $testDataRoot -Recurse -Force
}

# Clear the environment variable override
$env:UAM_DATA_DIR = $null

Write-Host "Cleanup complete. You can now close this terminal." -ForegroundColor Cyan
