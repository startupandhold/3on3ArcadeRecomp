# NHL 3 on 3 launcher: edits nhl3on3.toml beside nhl3on3.exe and starts the
# game. Lines in the file that the launcher doesn't manage are kept as they
# are, and settings left at their defaults are omitted from the file.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$Dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $Dir 'nhl3on3.exe'
$ConfigPath = Join-Path $Dir 'nhl3on3.toml'
$ArgsPath = Join-Path $Dir 'nhl3on3_launcher_args.txt'

# Files unpacked from a downloaded zip carry the "downloaded from the internet"
# mark, which makes Windows ask before every start of the unsigned game.
Get-ChildItem -LiteralPath $Dir -File |
  Where-Object { $_.Extension -in '.exe', '.dll' } |
  Unblock-File -ErrorAction SilentlyContinue

# Kind: bool | choice (fixed list) | edit (list or typed value) | text | size
# Toml: how the value is written (bool, int, string).
# Choices: value = label pairs, in display order.
$Settings = @(
  @{ Tab = 'Display'; Key = 'fullscreen'; Label = 'Fullscreen'; Kind = 'bool'; Toml = 'bool'; Default = 'true' }
  @{ Tab = 'Display'; Key = 'window_size'; Label = 'Window size (windowed)'; Kind = 'size'; Default = '0x0'
     Choices = [ordered]@{ '0x0' = 'Default'; '1280x720' = '1280 x 720'; '1600x900' = '1600 x 900'; '1920x1080' = '1920 x 1080'; '2560x1440' = '2560 x 1440' }
     Tip = 'Startup window size when not fullscreen.' }
  @{ Tab = 'Display'; Key = 'monitor'; Label = 'Monitor'; Kind = 'choice'; Toml = 'int'; Default = '0'
     Choices = [ordered]@{ '0' = 'Default'; '1' = '1 (primary)'; '2' = '2'; '3' = '3'; '4' = '4' } }
  @{ Tab = 'Display'; Key = 'aspect_ratio'; Label = 'Aspect ratio'; Kind = 'edit'; Toml = 'string'; Default = '16:9'
     Choices = [ordered]@{ '16:9' = '16:9'; '21:9' = '21:9'; '32:9' = '32:9'; 'auto' = 'auto' }
     Tip = "Wider than 16:9 widens the in-game camera (ultrawide).`n'auto' matches the desktop; you can also type W:H or a resolution such as 3440x1440." }
  @{ Tab = 'Display'; Key = 'portrait_camera'; Label = 'Portrait: match camera'; Kind = 'choice'; Toml = 'string'; Default = 'topdown'
     Choices = [ordered]@{ 'topdown' = 'Top-down (whole rink)'; 'classic' = 'Classic (tilted, follows play)'; 'game' = 'Game camera' }
     Tip = "Portrait aspect ratios only. Classic is closer and more tilted, like old-school hockey games,`nand scrolls up and down the rink with the play." }
  @{ Tab = 'Display'; Key = 'fps_cap'; Label = 'Frame rate cap'; Kind = 'edit'; Toml = 'string'; Default = '60'
     Choices = [ordered]@{ '60' = '60'; '90' = '90'; '120' = '120'; '144' = '144'; '165' = '165'; '240' = '240'; 'unlocked' = 'unlocked' }
     Tip = "60 is stock. Any value from 24 to 240, or 'unlocked'." }
  @{ Tab = 'Display'; Key = 'vsync'; Label = 'VSync'; Kind = 'bool'; Toml = 'bool'; Default = 'true'
     Tip = 'Off gives lower input latency and smoother pacing here; may tear without a variable refresh rate display.' }

  @{ Tab = 'Graphics'; Key = 'render_quality'; Label = 'Internal resolution'; Kind = 'choice'; Toml = 'string'; Default = ''
     Choices = [ordered]@{ '' = 'Native (720p)'; '2x' = '2x (1440p)'; '3x' = '3x (2160p)' } }
  @{ Tab = 'Graphics'; Key = 'anisotropic_override'; Label = 'Anisotropic filtering'; Kind = 'choice'; Toml = 'int'; Default = '3'
     Choices = [ordered]@{ '-1' = 'Game default'; '0' = 'Off'; '1' = '1x'; '2' = '2x'; '3' = '4x'; '4' = '8x'; '5' = '16x' } }
  @{ Tab = 'Graphics'; Key = 'native_2x_msaa'; Label = 'Native 2x MSAA'; Kind = 'bool'; Toml = 'bool'; Default = 'true' }

  @{ Tab = 'Game'; Key = 'skip_intro'; Label = 'Skip intro movie'; Kind = 'bool'; Toml = 'bool'; Default = 'true' }
  @{ Tab = 'Game'; Key = 'audio_mute'; Label = 'Mute audio'; Kind = 'bool'; Toml = 'bool'; Default = 'false' }
  @{ Tab = 'Game'; Key = 'input_backend'; Label = 'Controller input'; Kind = 'choice'; Toml = 'string'; Default = 'sdl'
     Choices = [ordered]@{ 'sdl' = 'SDL'; 'xinput' = 'XInput' } }
  @{ Tab = 'Game'; Key = 'mnk_mode'; Label = 'Keyboard as controller'; Kind = 'bool'; Toml = 'bool'; Default = 'false' }

  @{ Tab = 'VR'; Key = 'vr'; Label = 'VR headset (OpenXR)'; Kind = 'bool'; Toml = 'bool'; Default = 'false'
     Tip = "Matches in 3D on a tabletop rink, menus on a virtual screen.`nNeeds an OpenXR runtime (Virtual Desktop VDXR, SteamVR, Oculus) and runs at 120 FPS (60 per eye)." }

  @{ Tab = 'Online'; Key = 'online'; Label = 'Online play'; Kind = 'bool'; Toml = 'bool'; Default = 'false' }
  @{ Tab = 'Online'; Key = 'online_gamertag'; Label = 'Gamertag'; Kind = 'text'; Toml = 'string'; Default = ''
     Tip = 'Up to 15 characters; empty uses the profile name.' }
  @{ Tab = 'Online'; Key = 'online_server'; Label = 'Server'; Kind = 'text'; Toml = 'string'; Default = 'nhl3on3-turtle.fly.dev'
     Tip = 'Host name or IP address of the NHL 3 on 3 server.' }
  @{ Tab = 'Online'; Key = 'online_tunnel'; Label = 'Connect through server'; Kind = 'bool'; Toml = 'bool'; Default = 'true'
     Tip = "Sends game traffic through the server so no ports need forwarding.`nTurn off only to connect to players directly (LAN or VPN)." }
  @{ Tab = 'Online'; Key = 'online_ip'; Label = 'Advertised IP'; Kind = 'text'; Toml = 'string'; Default = ''
     Tip = "Only used when not connecting through the server.`nEmpty detects the LAN address; set a VPN address (Radmin, ZeroTier) to play over it." }
)

# --- nhl3on3.toml ---------------------------------------------------------

$KeyPattern = '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$'

function Read-Config {
  $values = @{}
  if (Test-Path $ConfigPath) {
    foreach ($line in Get-Content -LiteralPath $ConfigPath) {
      if ($line -match $KeyPattern) {
        $key = $Matches[1]
        $raw = $Matches[2]
        if ($raw -match '^"(.*)"$') {
          $raw = $Matches[1] -replace '\\"', '"' -replace '\\\\', '\'
        }
        $values[$key] = $raw
      }
    }
  }
  return $values
}

function Format-Value($value, $toml) {
  switch ($toml) {
    'string' { return '"' + ($value -replace '\\', '\\' -replace '"', '\"') + '"' }
    default { return $value }
  }
}

function Write-Config($values) {
  # team_play was a launcher setting; the server now sizes team play games itself.
  $managed = @{ 'team_play' = $true }
  foreach ($s in $Settings) {
    if ($s.Kind -eq 'size') {
      $managed['window_width'] = $true
      $managed['window_height'] = $true
    } else {
      $managed[$s.Key] = $true
    }
  }
  $lines = New-Object System.Collections.Generic.List[string]
  if (Test-Path $ConfigPath) {
    foreach ($line in Get-Content -LiteralPath $ConfigPath) {
      if ($line -match $KeyPattern -and $managed.ContainsKey($Matches[1])) {
        continue
      }
      if ($line -eq '# Set by the NHL3on3 launcher') {
        continue
      }
      $lines.Add($line)
    }
  }
  while ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -eq '') {
    $lines.RemoveAt($lines.Count - 1)
  }
  $ours = New-Object System.Collections.Generic.List[string]
  foreach ($s in $Settings) {
    $value = $values[$s.Key]
    if ($null -eq $value -or $value -eq $s.Default) {
      continue
    }
    if ($s.Kind -eq 'size') {
      $wh = $value -split 'x'
      $ours.Add("window_width = $($wh[0])")
      $ours.Add("window_height = $($wh[1])")
    } else {
      $ours.Add("$($s.Key) = $(Format-Value $value $s.Toml)")
    }
  }
  if ($ours.Count -gt 0) {
    if ($lines.Count -gt 0) {
      $lines.Add('')
    }
    $lines.Add('# Set by the NHL3on3 launcher')
    $lines.AddRange($ours)
  }
  $text = ($lines -join "`r`n")
  if ($text) {
    $text += "`r`n"
  }
  [System.IO.File]::WriteAllText($ConfigPath, $text, (New-Object System.Text.UTF8Encoding $false))
}

# --- Window ---------------------------------------------------------------

$form = New-Object System.Windows.Forms.Form
$form.Text = 'NHL 3 on 3 Arcade'
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
$form.AutoScaleDimensions = New-Object System.Drawing.SizeF(96, 96)
$form.ClientSize = New-Object System.Drawing.Size(440, 370)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)
if (Test-Path $Exe) {
  try { $form.Icon = [System.Drawing.Icon]::ExtractAssociatedIcon($Exe) } catch {}
}

$tip = New-Object System.Windows.Forms.ToolTip
$tip.AutoPopDelay = 15000

$tabs = New-Object System.Windows.Forms.TabControl
$tabs.Location = New-Object System.Drawing.Point(8, 8)
$tabs.Size = New-Object System.Drawing.Size(424, 276)
$form.Controls.Add($tabs)

$Controls = @{}
$Grids = @{}
foreach ($s in $Settings) {
  if (-not $Grids.ContainsKey($s.Tab)) {
    $page = New-Object System.Windows.Forms.TabPage
    $page.Text = $s.Tab
    $page.Padding = New-Object System.Windows.Forms.Padding(8)
    $grid = New-Object System.Windows.Forms.TableLayoutPanel
    $grid.Dock = 'Fill'
    $grid.ColumnCount = 2
    [void]$grid.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', 160)))
    [void]$grid.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Percent', 100)))
    $grid.AutoScroll = $true
    $page.Controls.Add($grid)
    $tabs.TabPages.Add($page)
    $Grids[$s.Tab] = $grid
  }
  $grid = $Grids[$s.Tab]

  $label = New-Object System.Windows.Forms.Label
  $label.Text = $s.Label
  $label.AutoSize = $true
  $label.Anchor = 'Left'
  $label.Margin = New-Object System.Windows.Forms.Padding(0, 6, 0, 6)

  switch ($s.Kind) {
    'bool' {
      $c = New-Object System.Windows.Forms.CheckBox
      $c.AutoSize = $true
    }
    'text' {
      $c = New-Object System.Windows.Forms.TextBox
      $c.Width = 220
    }
    default {
      $c = New-Object System.Windows.Forms.ComboBox
      $c.Width = 220
      if ($s.Kind -eq 'edit') {
        $c.DropDownStyle = 'DropDown'
      } else {
        $c.DropDownStyle = 'DropDownList'
      }
      foreach ($label_text in $s.Choices.Values) {
        [void]$c.Items.Add($label_text)
      }
    }
  }
  $c.Anchor = 'Left'
  if ($s.Tip) {
    $tip.SetToolTip($label, $s.Tip)
    $tip.SetToolTip($c, $s.Tip)
  }
  $row = $grid.RowCount
  $grid.RowCount = $row + 1
  [void]$grid.RowStyles.Add((New-Object System.Windows.Forms.RowStyle('AutoSize')))
  $grid.Controls.Add($label, 0, $row)
  $grid.Controls.Add($c, 1, $row)
  $Controls[$s.Key] = $c
}
foreach ($grid in $Grids.Values) {
  $grid.RowCount += 1
  [void]$grid.RowStyles.Add((New-Object System.Windows.Forms.RowStyle('Percent', 100)))
}

$argsLabel = New-Object System.Windows.Forms.Label
$argsLabel.Text = 'Extra arguments'
$argsLabel.AutoSize = $true
$argsLabel.Location = New-Object System.Drawing.Point(12, 296)
$form.Controls.Add($argsLabel)

$argsBox = New-Object System.Windows.Forms.TextBox
$argsBox.Location = New-Object System.Drawing.Point(112, 292)
$argsBox.Width = 316
$tip.SetToolTip($argsBox, 'Passed to nhl3on3.exe as-is, for example --log_fps=true --latency_probe=true')
$form.Controls.Add($argsBox)

function Set-Control($s, $value) {
  $c = $Controls[$s.Key]
  switch ($s.Kind) {
    'bool' { $c.Checked = ($value -eq 'true') }
    'text' { $c.Text = $value }
    default {
      if ($s.Choices.Contains($value)) {
        $c.SelectedItem = $s.Choices[$value]
      } elseif ($s.Kind -eq 'edit') {
        $c.Text = $value
      } else {
        $c.SelectedItem = $s.Choices[$s.Default]
      }
    }
  }
}

function Get-Control($s) {
  $c = $Controls[$s.Key]
  switch ($s.Kind) {
    'bool' { if ($c.Checked) { return 'true' } else { return 'false' } }
    'text' { return $c.Text.Trim() }
    default {
      $text = $c.Text.Trim()
      foreach ($entry in $s.Choices.GetEnumerator()) {
        if ($entry.Value -eq $text) {
          return $entry.Key
        }
      }
      if ($s.Kind -eq 'edit' -and $text) {
        return $text
      }
      return $s.Default
    }
  }
}

function Load-Form {
  $values = Read-Config
  foreach ($s in $Settings) {
    if ($s.Kind -eq 'size') {
      $value = '{0}x{1}' -f $(if ($values['window_width']) { $values['window_width'] } else { '0' }),
                           $(if ($values['window_height']) { $values['window_height'] } else { '0' })
    } elseif ($values.ContainsKey($s.Key)) {
      $value = $values[$s.Key]
    } else {
      $value = $s.Default
    }
    Set-Control $s $value
  }
  if (Test-Path $ArgsPath) {
    $argsBox.Text = (Get-Content -LiteralPath $ArgsPath -Raw).Trim()
  }
}

function Save-Form {
  $values = @{}
  foreach ($s in $Settings) {
    $values[$s.Key] = Get-Control $s
  }
  Write-Config $values
  $extra = $argsBox.Text.Trim()
  if ($extra) {
    [System.IO.File]::WriteAllText($ArgsPath, $extra)
  } elseif (Test-Path $ArgsPath) {
    Remove-Item -LiteralPath $ArgsPath
  }
}

function New-Button($text, $x, $width) {
  $b = New-Object System.Windows.Forms.Button
  $b.Text = $text
  $b.Location = New-Object System.Drawing.Point($x, 330)
  $b.Size = New-Object System.Drawing.Size($width, 30)
  $form.Controls.Add($b)
  return $b
}

$defaultsButton = New-Button 'Defaults' 8 80
$defaultsButton.Add_Click({
  foreach ($s in $Settings) {
    Set-Control $s $s.Default
  }
  $argsBox.Text = ''
})

$saveButton = New-Button 'Save' 254 80
$saveButton.Add_Click({
  try {
    Save-Form
  } catch {
    [System.Windows.Forms.MessageBox]::Show("Couldn't save settings: $_", $form.Text) | Out-Null
  }
})

$playButton = New-Button 'Play' 342 90
$playButton.Font = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold)
$playButton.Add_Click({
  if (-not (Test-Path $Exe)) {
    [System.Windows.Forms.MessageBox]::Show("nhl3on3.exe not found in $Dir", $form.Text) | Out-Null
    return
  }
  try {
    Save-Form
    $params = @{ FilePath = $Exe; WorkingDirectory = $Dir }
    if ($argsBox.Text.Trim()) {
      $params.ArgumentList = $argsBox.Text.Trim()
    }
    Start-Process @params
    $form.Close()
  } catch {
    [System.Windows.Forms.MessageBox]::Show("Couldn't start the game: $_", $form.Text) | Out-Null
  }
})
$form.AcceptButton = $playButton

Load-Form
[void]$form.ShowDialog()
