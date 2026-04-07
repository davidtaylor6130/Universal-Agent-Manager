# build_gui_selector.ps1
# Windows GUI selector for choosing which providers to enable in the build.
# Uses native Windows Forms — no dependencies required.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.Text = "UAM Provider Build Selector"
$form.Size = New-Object System.Drawing.Size(420, 480)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.MinimizeBox = $false

$label = New-Object System.Windows.Forms.Label
$label.Text = "Select which providers to include in this build:"
$label.Location = New-Object System.Drawing.Point(15, 12)
$label.Size = New-Object System.Drawing.Size(370, 20)
$label.Font = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Regular)
$form.Controls.Add($label)

$providers = @(
    @{ Name = "Gemini (Structured)";      Checked = $true; Flag = "G_S" }
    @{ Name = "Gemini (CLI)";             Checked = $true; Flag = "G_C" }
    @{ Name = "Claude Code";              Checked = $true; Flag = "CL" }
    @{ Name = "Codex CLI";                Checked = $true; Flag = "CX" }
    @{ Name = "OpenCode CLI";             Checked = $true; Flag = "OC" }
    @{ Name = "OpenCode Local";           Checked = $true; Flag = "OL" }
    @{ Name = "Ollama Engine (Local)";    Checked = $true; Flag = "OE" }
)

$y = 38
$checkBoxes = @{}
foreach ($p in $providers) {
    $cb = New-Object System.Windows.Forms.CheckBox
    $cb.Text = $p.Name
    $cb.Location = New-Object System.Drawing.Point(25, $y)
    $cb.Size = New-Object System.Drawing.Size(350, 24)
    $cb.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Regular)
    $cb.Checked = $p.Checked
    $form.Controls.Add($cb)
    $checkBoxes[$p.Flag] = $cb
    $y += 28
}

$depLabel = New-Object System.Windows.Forms.Label
$depLabel.Text = ""
$depLabel.Location = New-Object System.Drawing.Point(15, 250)
$depLabel.Size = New-Object System.Drawing.Size(370, 50)
$depLabel.Font = New-Object System.Drawing.Font("Segoe UI", 8.5, [System.Drawing.FontStyle]::Italic)
$depLabel.ForeColor = [System.Drawing.Color]::DarkOrange
$form.Controls.Add($depLabel)

$btnBuild = New-Object System.Windows.Forms.Button
$btnBuild.Text = "Build"
$btnBuild.Location = New-Object System.Drawing.Point(170, 320)
$btnBuild.Size = New-Object System.Drawing.Size(90, 32)
$btnBuild.DialogResult = [System.Windows.Forms.DialogResult]::OK
$form.AcceptButton = $btnBuild
$form.Controls.Add($btnBuild)

$btnCancel = New-Object System.Windows.Forms.Button
$btnCancel.Text = "Cancel"
$btnCancel.Location = New-Object System.Drawing.Point(270, 320)
$btnCancel.Size = New-Object System.Drawing.Size(90, 32)
$btnCancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$form.CancelButton = $btnCancel
$form.Controls.Add($btnCancel)

# Dependency enforcement on checkbox change
$enforceDeps = {
    $msgs = @()
    if ($checkBoxes["OL"].Checked) {
        if (-not $checkBoxes["OC"].Checked) {
            $checkBoxes["OC"].Checked = $true
            $msgs += "• OpenCode Local requires OpenCode CLI — enabled."
        }
        if (-not $checkBoxes["OE"].Checked) {
            $checkBoxes["OE"].Checked = $true
            $msgs += "• OpenCode Local requires Ollama Engine — enabled."
        }
    }
    $depLabel.Text = ($msgs -join "`n")
}

foreach ($cb in $checkBoxes.Values) {
    $cb.Add_CheckChanged($enforceDeps)
}

if ($form.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
    Write-Host "Build cancelled."
    exit 0
}

$G_S  = if ($checkBoxes["G_S"].Checked)  { "ON" } else { "OFF" }
$G_C  = if ($checkBoxes["G_C"].Checked)  { "ON" } else { "OFF" }
$CL   = if ($checkBoxes["CL"].Checked)   { "ON" } else { "OFF" }
$CX   = if ($checkBoxes["CX"].Checked)   { "ON" } else { "OFF" }
$OC   = if ($checkBoxes["OC"].Checked)   { "ON" } else { "OFF" }
$OL   = if ($checkBoxes["OL"].Checked)   { "ON" } else { "OFF" }
$OE   = if ($checkBoxes["OE"].Checked)   { "ON" } else { "OFF" }

$count = 0
foreach ($v in $G_S, $G_C, $CL, $CX, $OC, $OL, $OE) { if ($v -eq "ON") { $count++ } }
if ($count -eq 0) {
    [System.Windows.Forms.MessageBox]::Show("At least one provider must be selected.", "UAM Build Selector", "OK", "Error")
    exit 1
}

$nameParts = @()
if ($G_S -eq "ON") { $nameParts += "GeminiS" }
if ($G_C -eq "ON") { $nameParts += "GeminiC" }
if ($CL  -eq "ON") { $nameParts += "Claude" }
if ($CX  -eq "ON") { $nameParts += "Codex" }
if ($OC  -eq "ON") { $nameParts += "OpenCodeCLI" }
if ($OL  -eq "ON") { $nameParts += "OpenCodeLocal" }
if ($OE  -eq "ON") { $nameParts += "OllamaEngine" }

$configName = $nameParts -join "+"
$outDir = "Builds\ProviderFlags\$configName"

Write-Host ""
Write-Host "=========================================="
Write-Host "Configuration: $configName"
Write-Host "Output:        $outDir"
Write-Host "=========================================="
Write-Host ""
Write-Host "  Gemini (Structured): $G_S"
Write-Host "  Gemini (CLI):        $G_C"
Write-Host "  Claude Code:         $CL"
Write-Host "  Codex CLI:           $CX"
Write-Host "  OpenCode CLI:        $OC"
Write-Host "  OpenCode Local:      $OL"
Write-Host "  Ollama Engine:       $OE"
Write-Host ""

& cmake -S . -B $outDir `
    -DUAM_FETCH_DEPS=ON `
    -DUAM_FETCH_LLAMA_CPP=OFF `
    -DUAM_BUILD_TESTS=OFF `
    -DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=$G_S `
    -DUAM_ENABLE_RUNTIME_GEMINI_CLI=$G_C `
    -DUAM_ENABLE_RUNTIME_CODEX_CLI=$CX `
    -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=$CL `
    -DUAM_ENABLE_RUNTIME_OPENCODE_CLI=$OC `
    -DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL=$OL `
    -DUAM_ENABLE_RUNTIME_OLLAMA_ENGINE=$OE

if ($LASTEXITCODE -ne 0) { exit 1 }

& cmake --build $outDir -j8

if ($LASTEXITCODE -ne 0) { exit 1 }

$binary = "$outDir\universal_agent_manager.exe"
Write-Host ""
Write-Host "Build complete: $configName"
Write-Host "Binary: $binary"
Write-Host ""

[System.Windows.Forms.MessageBox]::Show("Build complete!`n`nConfiguration: $configName`nBinary: $binary", "UAM Build Complete", "OK", "Information")
