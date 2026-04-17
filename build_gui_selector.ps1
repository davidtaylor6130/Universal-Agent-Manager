# build_gui_selector.ps1
# Windows GUI launcher for the Gemini CLI release-slice build.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.Text = "UAM Gemini CLI Build"
$form.Size = New-Object System.Drawing.Size(420, 220)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.MinimizeBox = $false

$label = New-Object System.Windows.Forms.Label
$label.Text = "Build Universal Agent Manager for the Gemini CLI release slice."
$label.Location = New-Object System.Drawing.Point(15, 18)
$label.Size = New-Object System.Drawing.Size(370, 40)
$label.Font = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Regular)
$form.Controls.Add($label)

$details = New-Object System.Windows.Forms.Label
$details.Text = "Unsupported provider variants are intentionally removed from this build."
$details.Location = New-Object System.Drawing.Point(15, 65)
$details.Size = New-Object System.Drawing.Size(370, 35)
$details.Font = New-Object System.Drawing.Font("Segoe UI", 8.5, [System.Drawing.FontStyle]::Italic)
$details.ForeColor = [System.Drawing.Color]::DarkOrange
$form.Controls.Add($details)

$btnBuild = New-Object System.Windows.Forms.Button
$btnBuild.Text = "Build"
$btnBuild.Location = New-Object System.Drawing.Point(205, 125)
$btnBuild.Size = New-Object System.Drawing.Size(90, 32)
$btnBuild.DialogResult = [System.Windows.Forms.DialogResult]::OK
$form.AcceptButton = $btnBuild
$form.Controls.Add($btnBuild)

$btnCancel = New-Object System.Windows.Forms.Button
$btnCancel.Text = "Cancel"
$btnCancel.Location = New-Object System.Drawing.Point(305, 125)
$btnCancel.Size = New-Object System.Drawing.Size(90, 32)
$btnCancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$form.CancelButton = $btnCancel
$form.Controls.Add($btnCancel)

if ($form.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
    Write-Host "Build cancelled."
    exit 0
}

$configName = "GeminiCLI"
$outDir = "Builds\GeminiCLI"

Write-Host ""
Write-Host "=========================================="
Write-Host "Configuration: $configName"
Write-Host "Output:        $outDir"
Write-Host "=========================================="
Write-Host ""

& cmake -S . -B $outDir `
    -DUAM_FETCH_DEPS=ON `
    -DUAM_BUILD_TESTS=OFF

if ($LASTEXITCODE -ne 0) { exit 1 }

& cmake --build $outDir -j8

if ($LASTEXITCODE -ne 0) { exit 1 }

$binary = "$outDir\Release\universal_agent_manager.exe"
Write-Host ""
Write-Host "Build complete: $configName"
Write-Host "Binary: $binary"
Write-Host ""

[System.Windows.Forms.MessageBox]::Show("Build complete!`n`nConfiguration: $configName`nBinary: $binary", "UAM Build Complete", "OK", "Information")
