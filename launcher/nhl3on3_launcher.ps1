# NHL 3 on 3 launcher: edits nhl3on3.toml beside nhl3on3.exe and starts the
# game. Lines in the file that the launcher doesn't manage are kept as they
# are, and settings left at their defaults are omitted from the file.
# It also updates the whole folder (game and launcher) from the latest GitHub
# release; see Updates below.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.Windows.Forms.Application]::EnableVisualStyles()

$ScriptPath = $MyInvocation.MyCommand.Path
$Dir = Split-Path -Parent $ScriptPath
$Exe = Join-Path $Dir 'nhl3on3.exe'
$ConfigPath = Join-Path $Dir 'nhl3on3.toml'
$ArgsPath = Join-Path $Dir 'nhl3on3_launcher_args.txt'
$VersionPath = Join-Path $Dir 'version.txt'

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
     Choices = [ordered]@{ '60' = '60'; '90' = '90'; '120' = '120'; '144' = '144'; '165' = '165'; '240' = '240'; 'display' = 'display'; 'unlocked' = 'unlocked' }
     Tip = "60 is stock. Any value from 24 to 500, 'display' (your display's refresh rate),`nor 'unlocked' (as fast as your PC can render). Matches run at half the cap or less." }
  @{ Tab = 'Display'; Key = 'vsync'; Label = 'VSync'; Kind = 'bool'; Toml = 'bool'; Default = 'true'
     Tip = 'Shows each frame in sync with the display, so the picture never tears. Off has slightly lower latency but may tear without a G-Sync / FreeSync display. Does not change the frame rate cap.' }

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

# --- Updates --------------------------------------------------------------

# Release folders carry version.txt (scripts/package_release.sh), the tag
# they were built from. A folder without it (a build folder) never updates,
# so the launcher can't overwrite a developer's build with a release.
$Repo = 'startupandhold/3on3ArcadeRecomp'
$Version = ''
if (Test-Path $VersionPath) {
  $Version = (Get-Content -LiteralPath $VersionPath -Raw).Trim()
}
[Net.ServicePointManager]::SecurityProtocol =
  [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# State: idle | checking | available | downloading; the timer drives the
# asynchronous steps so no event handler runs off the UI thread.
$Update = @{ State = 'idle'; Task = $null; Client = $null; Tag = ''; Zip = $null; Sums = $null; Size = 0; File = '' }

function New-WebClient {
  $wc = New-Object System.Net.WebClient
  $wc.Headers.Add('User-Agent', 'NHL3on3-launcher')
  return $wc
}

# True when release tag $tag is newer than $current (numbers compared in order).
function Test-Newer($tag, $current) {
  $a = @([regex]::Matches($tag, '\d+') | ForEach-Object { [int]$_.Value })
  $b = @([regex]::Matches($current, '\d+') | ForEach-Object { [int]$_.Value })
  if ($a.Count -eq 0 -or $b.Count -eq 0) {
    return $tag -ne $current
  }
  for ($i = 0; $i -lt [Math]::Max($a.Count, $b.Count); ++$i) {
    $x = if ($i -lt $a.Count) { $a[$i] } else { 0 }
    $y = if ($i -lt $b.Count) { $b[$i] } else { 0 }
    if ($x -ne $y) {
      return $x -gt $y
    }
  }
  return $false
}

# Unpacks the downloaded zip and copies it over this folder. Settings, logs
# and game files aren't in the zip, so they stay. version.txt goes last, so
# an update that fails partway is offered again.
function Install-Update($zip) {
  $running = Get-Process -Name 'nhl3on3' -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $Exe }
  if ($running) {
    throw 'Close the game first.'
  }
  $staging = Join-Path ([IO.Path]::GetTempPath()) ('nhl3on3-update-' + [guid]::NewGuid())
  try {
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $staging)
    $root = Get-ChildItem -LiteralPath $staging -Directory | Select-Object -First 1
    if (-not $root -or -not (Test-Path (Join-Path $root.FullName 'nhl3on3.exe'))) {
      throw "The download doesn't contain nhl3on3.exe."
    }
    $files = Get-ChildItem -LiteralPath $root.FullName -Recurse -File |
      Sort-Object { $_.Name -eq 'version.txt' }
    foreach ($f in $files) {
      $dest = Join-Path $Dir $f.FullName.Substring($root.FullName.Length + 1)
      New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
      Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
    }
  } finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
  }
}

# Checks the download against the release's SHA256SUMS.
function Test-Download($zip, $sumsUrl, $name) {
  $sums = (New-WebClient).DownloadString($sumsUrl)
  $want = $null
  foreach ($line in $sums -split "`n") {
    if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$' -and $Matches[2] -eq $name) {
      $want = $Matches[1]
    }
  }
  if (-not $want) {
    throw "The release has no checksum for $name."
  }
  # Antivirus can block reading the file; say so rather than call it damaged.
  try {
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256 -ErrorAction Stop).Hash
  } catch {
    throw "Couldn't read the download: $($_.Exception.Message)"
  }
  if ($hash -ne $want.ToUpperInvariant()) {
    throw 'The download is damaged (checksum mismatch). Try again.'
  }
}

# --- Theme ----------------------------------------------------------------

# Arena dark, ice white, and the rink's red line and blue lines.
function Rgb($r, $g, $b) { return [System.Drawing.Color]::FromArgb($r, $g, $b) }
$Theme = @{
  Back = Rgb 10 20 34; Panel = Rgb 18 34 58; Input = Rgb 11 25 44; Line = Rgb 38 66 99
  Text = Rgb 238 244 250; Muted = Rgb 141 162 186; Ice = Rgb 150 205 255
  Red = Rgb 215 38 61; RedHover = Rgb 236 58 82
  Blue = Rgb 31 111 209; BlueHover = Rgb 58 134 230
  Button = Rgb 27 48 80; ButtonHover = Rgb 38 66 106
}
$UiFont = New-Object System.Drawing.Font('Segoe UI', 9)
$BoldFont = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold)
$TabFont = New-Object System.Drawing.Font('Segoe UI Black', 8.5, [System.Drawing.FontStyle]::Italic)
$ButtonFont = New-Object System.Drawing.Font('Segoe UI Black', 9, [System.Drawing.FontStyle]::Italic)

function Set-ButtonStyle($b, $back, $hover, $fore) {
  $b.FlatStyle = 'Flat'
  $b.FlatAppearance.BorderSize = 0
  $b.FlatAppearance.MouseOverBackColor = $hover
  $b.FlatAppearance.MouseDownBackColor = $hover
  $b.BackColor = $back
  $b.ForeColor = $fore
  $b.Font = $ButtonFont
  $b.Cursor = [System.Windows.Forms.Cursors]::Hand
  $b.UseVisualStyleBackColor = $false
}

# --- Window ---------------------------------------------------------------

$form = New-Object System.Windows.Forms.Form
$form.Text = 'NHL 3 on 3 Arcade'
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
$form.AutoScaleDimensions = New-Object System.Drawing.SizeF(96, 96)
$form.ClientSize = New-Object System.Drawing.Size(452, 474)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = $UiFont
$form.BackColor = $Theme.Back
$form.ForeColor = $Theme.Text
if (Test-Path $Exe) {
  try { $form.Icon = [System.Drawing.Icon]::ExtractAssociatedIcon($Exe) } catch {}
}

$tip = New-Object System.Windows.Forms.ToolTip
$tip.AutoPopDelay = 15000

# Docked panels lay out from the last added: header, update banner and tab
# strip on top, buttons at the bottom, the settings filling the rest.
$content = New-Object System.Windows.Forms.Panel
$content.Dock = 'Fill'
$content.BackColor = $Theme.Panel
$content.Padding = New-Object System.Windows.Forms.Padding(12, 10, 12, 6)
$form.Controls.Add($content)

$footer = New-Object System.Windows.Forms.Panel
$footer.Dock = 'Bottom'
$footer.Height = 92
$form.Controls.Add($footer)

$strip = New-Object System.Windows.Forms.FlowLayoutPanel
$strip.Dock = 'Top'
$strip.Height = 34
$strip.Padding = New-Object System.Windows.Forms.Padding(8, 0, 0, 0)
$strip.WrapContents = $false
$form.Controls.Add($strip)

$banner = New-Object System.Windows.Forms.Panel
$banner.Dock = 'Top'
$banner.Height = 40
$banner.BackColor = $Theme.Blue
$banner.Padding = New-Object System.Windows.Forms.Padding(12, 6, 8, 6)
$banner.Visible = $false
$bannerText = New-Object System.Windows.Forms.Label
$bannerText.Dock = 'Fill'
$bannerText.TextAlign = 'MiddleLeft'
$bannerText.Font = $BoldFont
$bannerText.ForeColor = [System.Drawing.Color]::White
$banner.Controls.Add($bannerText)
$updateButton = New-Object System.Windows.Forms.Button
$updateButton.Dock = 'Right'
$updateButton.Width = 104
$updateButton.Text = 'UPDATE'
Set-ButtonStyle $updateButton ([System.Drawing.Color]::White) $Theme.Text $Theme.Back
$banner.Controls.Add($updateButton)
$form.Controls.Add($banner)

$header = New-Object System.Windows.Forms.Panel
$header.Dock = 'Top'
$header.Height = 74
$form.Controls.Add($header)

$header.Add_Paint({
  $g = $_.Graphics
  $g.SmoothingMode = 'AntiAlias'
  $g.TextRenderingHint = 'AntiAliasGridFit'
  $w = $header.ClientSize.Width
  $h = $header.ClientSize.Height
  $s = $g.DpiX / 96
  $rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
  $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, (Rgb 22 44 76), $Theme.Back, 90)
  $g.FillRectangle($fill, $rect)

  # A faint half rink at the right: boards, blue line, faceoff circle, crease.
  $pen = New-Object System.Drawing.Pen((Rgb 34 62 98), (2 * $s))
  $rx = $w - 150 * $s
  $g.DrawArc($pen, $rx, 10 * $s, 120 * $s, $h - 22 * $s, -90, 180)
  $g.DrawLine($pen, $rx + 60 * $s, 10 * $s, $rx + 60 * $s, $h - 12 * $s)
  $pen.Color = Rgb 26 66 122
  $g.DrawLine($pen, $rx + 30 * $s, 10 * $s, $rx + 30 * $s, $h - 12 * $s)
  $pen.Color = Rgb 92 34 52
  $g.DrawEllipse($pen, $rx + 76 * $s, $h / 2 - 16 * $s, 32 * $s, 32 * $s)

  $x = 16 * $s
  $small = New-Object System.Drawing.Font('Segoe UI Black', 10, [System.Drawing.FontStyle]::Italic)
  $big = New-Object System.Drawing.Font('Segoe UI Black', 20, [System.Drawing.FontStyle]::Italic)
  $red = New-Object System.Drawing.SolidBrush($Theme.Red)
  $white = New-Object System.Drawing.SolidBrush($Theme.Text)
  $ice = New-Object System.Drawing.SolidBrush($Theme.Ice)
  $muted = New-Object System.Drawing.SolidBrush($Theme.Muted)
  $tight = [System.Drawing.StringFormat]::GenericTypographic
  $g.DrawString('NHL', $small, $red, $x, 9 * $s, $tight)
  $label = if ($Version) { $Version } else { 'dev build' }
  $g.DrawString($label, $UiFont, $muted, $x + $g.MeasureString('NHL', $small, 200, $tight).Width + 8 * $s, 10 * $s, $tight)
  $g.DrawString('3 ON 3', $big, $white, $x - 2 * $s, 26 * $s, $tight)
  $off = $g.MeasureString('3 ON 3', $big, 400, $tight).Width + 10 * $s
  $g.DrawString('ARCADE', $big, $ice, $x + $off, 26 * $s, $tight)

  # Rink markings along the bottom: blue line, red line, blue line.
  $band = [Math]::Max(3, [int](3 * $s))
  $blue = New-Object System.Drawing.SolidBrush($Theme.Blue)
  $g.FillRectangle($blue, 0, $h - $band, $w, $band)
  $g.FillRectangle($red, [int]($w / 3), $h - $band, [int]($w / 3), $band)
  foreach ($o in $fill, $pen, $small, $big, $red, $white, $ice, $muted, $blue) { $o.Dispose() }
})

# Tabs: a strip of buttons, each showing its page of settings.
$Controls = @{}
$Grids = [ordered]@{}
$TabButtons = @{}
$script:CurrentTab = $null

function Select-Tab($name) {
  $script:CurrentTab = $name
  foreach ($key in $Grids.Keys) {
    $Grids[$key].Visible = ($key -eq $name)
    $b = $TabButtons[$key]
    $b.BackColor = $(if ($key -eq $name) { $Theme.Panel } else { $Theme.Back })
    $b.ForeColor = $(if ($key -eq $name) { $Theme.Text } else { $Theme.Muted })
    $b.Invalidate()
  }
}

foreach ($s in $Settings) {
  if (-not $Grids.Contains($s.Tab)) {
    $grid = New-Object System.Windows.Forms.TableLayoutPanel
    $grid.Dock = 'Fill'
    $grid.ColumnCount = 2
    [void]$grid.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', 172)))
    [void]$grid.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Percent', 100)))
    $grid.AutoScroll = $true
    $grid.Visible = $false
    $content.Controls.Add($grid)
    $Grids[$s.Tab] = $grid

    $tb = New-Object System.Windows.Forms.Button
    $tb.Text = $s.Tab.ToUpperInvariant()
    $tb.Size = New-Object System.Drawing.Size(84, 34)
    $tb.Margin = New-Object System.Windows.Forms.Padding(0)
    Set-ButtonStyle $tb $Theme.Back $Theme.Panel $Theme.Muted
    $tb.Font = $TabFont
    $tb.Tag = $s.Tab
    $tb.Add_Click({ Select-Tab $this.Tag })
    # The current tab gets a red bar along its top edge.
    $tb.Add_Paint({
      if ($this.Tag -eq $script:CurrentTab) {
        $bar = New-Object System.Drawing.SolidBrush($Theme.Red)
        $_.Graphics.FillRectangle($bar, 0, 0, $this.Width, [Math]::Max(3, [int]($this.Height / 11)))
        $bar.Dispose()
      }
    })
    $strip.Controls.Add($tb)
    $TabButtons[$s.Tab] = $tb
  }
  $grid = $Grids[$s.Tab]

  $label = New-Object System.Windows.Forms.Label
  $label.Text = $s.Label
  $label.AutoSize = $true
  $label.Anchor = 'Left'
  $label.ForeColor = $Theme.Text
  $label.Margin = New-Object System.Windows.Forms.Padding(0, 7, 0, 7)

  switch ($s.Kind) {
    'bool' {
      # An ON / OFF toggle: flat check boxes can't be themed.
      $c = New-Object System.Windows.Forms.CheckBox
      $c.Appearance = 'Button'
      $c.Size = New-Object System.Drawing.Size(64, 26)
      $c.TextAlign = 'MiddleCenter'
      Set-ButtonStyle $c $Theme.Button $Theme.ButtonHover $Theme.Muted
      $c.FlatAppearance.CheckedBackColor = $Theme.Blue
      $c.Font = $TabFont
      $c.Text = 'OFF'
      $c.Add_CheckedChanged({
        $this.Text = $(if ($this.Checked) { 'ON' } else { 'OFF' })
        $this.ForeColor = $(if ($this.Checked) { [System.Drawing.Color]::White } else { $Theme.Muted })
      })
    }
    'text' {
      $c = New-Object System.Windows.Forms.TextBox
      $c.Width = 220
      $c.BorderStyle = 'FixedSingle'
    }
    default {
      $c = New-Object System.Windows.Forms.ComboBox
      $c.Width = 220
      $c.FlatStyle = 'Flat'
      if ($s.Kind -eq 'edit') {
        $c.DropDownStyle = 'DropDown'
      } else {
        # A fixed list ignores BackColor unless it draws its own items.
        $c.DropDownStyle = 'DropDownList'
        $c.DrawMode = 'OwnerDrawFixed'
        $c.Add_DrawItem({
          if ($_.Index -lt 0) { return }
          $on = ($_.State -band [System.Windows.Forms.DrawItemState]::Selected) -ne 0
          $brush = New-Object System.Drawing.SolidBrush($(if ($on) { $Theme.Blue } else { $Theme.Input }))
          $_.Graphics.FillRectangle($brush, $_.Bounds)
          $brush.Dispose()
          [System.Windows.Forms.TextRenderer]::DrawText($_.Graphics, [string]$this.Items[$_.Index], $this.Font,
            $_.Bounds, $Theme.Text, [System.Windows.Forms.TextFormatFlags]'Left, VerticalCenter')
        })
      }
      foreach ($label_text in $s.Choices.Values) {
        [void]$c.Items.Add($label_text)
      }
    }
  }
  if ($s.Kind -ne 'bool') {
    $c.BackColor = $Theme.Input
    $c.ForeColor = $Theme.Text
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
$argsLabel.ForeColor = $Theme.Muted
$argsLabel.Location = New-Object System.Drawing.Point(12, 15)
$footer.Controls.Add($argsLabel)

$argsBox = New-Object System.Windows.Forms.TextBox
$argsBox.Location = New-Object System.Drawing.Point(116, 12)
$argsBox.Width = 324
$argsBox.BorderStyle = 'FixedSingle'
$argsBox.BackColor = $Theme.Input
$argsBox.ForeColor = $Theme.Text
$tip.SetToolTip($argsBox, 'Passed to nhl3on3.exe as-is, for example --log_fps=true --latency_probe=true')
$footer.Controls.Add($argsBox)

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
  $b.Location = New-Object System.Drawing.Point($x, 48)
  $b.Size = New-Object System.Drawing.Size($width, 32)
  Set-ButtonStyle $b $Theme.Button $Theme.ButtonHover $Theme.Text
  $footer.Controls.Add($b)
  return $b
}

$defaultsButton = New-Button 'DEFAULTS' 12 90
$defaultsButton.ForeColor = $Theme.Muted
$defaultsButton.Add_Click({
  foreach ($s in $Settings) {
    Set-Control $s $s.Default
  }
  $argsBox.Text = ''
})

$saveButton = New-Button 'SAVE' 244 90
$saveButton.Add_Click({
  try {
    Save-Form
  } catch {
    [System.Windows.Forms.MessageBox]::Show("Couldn't save settings: $_", $form.Text) | Out-Null
  }
})

$playButton = New-Button 'PLAY' 340 100
Set-ButtonStyle $playButton $Theme.Red $Theme.RedHover ([System.Drawing.Color]::White)
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

# --- Update flow ----------------------------------------------------------

function Show-Banner($text, $button) {
  if (-not $banner.Visible) {
    $banner.Visible = $true
    $form.ClientSize = New-Object System.Drawing.Size($form.ClientSize.Width, ($form.ClientSize.Height + $banner.Height))
  }
  $bannerText.Text = $text
  $updateButton.Visible = [bool]$button
  if ($button) {
    $updateButton.Text = $button
  }
}

function Fail-Update($message) {
  $Update.State = 'available'
  $updateTimer.Stop()
  $playButton.Enabled = $true
  Show-Banner "Update failed: $message" 'RETRY'
}

$updateTimer = New-Object System.Windows.Forms.Timer
$updateTimer.Interval = 200
$updateTimer.Add_Tick({
  $task = $Update.Task
  if ($Update.State -eq 'checking' -and $task.IsCompleted) {
    $updateTimer.Stop()
    $Update.State = 'idle'
    # Offline or rate limited: say nothing, the launcher works as before.
    if ($task.IsFaulted -or $task.IsCanceled) {
      return
    }
    try {
      $release = $task.Result | ConvertFrom-Json
      $zip = $release.assets | Where-Object { $_.name -like '*-windows_64_portable.zip' } | Select-Object -First 1
      $sums = $release.assets | Where-Object { $_.name -like '*-SHA256SUMS.txt' } | Select-Object -First 1
      if ($zip -and $sums -and (Test-Newer $release.tag_name $Version)) {
        $Update.State = 'available'
        $Update.Tag = $release.tag_name
        $Update.Zip = $zip
        $Update.Sums = $sums
        Show-Banner "$($release.tag_name) is out (you have $Version)." 'UPDATE'
      }
    } catch {}
  } elseif ($Update.State -eq 'downloading') {
    if (-not $task.IsCompleted) {
      if ($Update.Size -gt 0 -and (Test-Path $Update.File)) {
        $pct = [int](100 * (Get-Item -LiteralPath $Update.File).Length / $Update.Size)
        Show-Banner "Downloading $($Update.Tag)... $pct%" $null
      }
      return
    }
    $updateTimer.Stop()
    if ($task.IsFaulted -or $task.IsCanceled) {
      $reason = if ($task.Exception) { $task.Exception.GetBaseException().Message } else { 'canceled' }
      Fail-Update $reason
      return
    }
    try {
      Show-Banner "Installing $($Update.Tag)..." $null
      $form.Refresh()
      Test-Download $Update.File $Update.Sums.browser_download_url $Update.Zip.name
      Install-Update $Update.File
      Remove-Item -LiteralPath $Update.File -Force -ErrorAction SilentlyContinue
      # Settings typed since the launcher opened go into the new version.
      try { Save-Form } catch {}
      Start-Process powershell.exe -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-WindowStyle', 'Hidden', '-File', "`"$ScriptPath`""
      $form.Close()
    } catch {
      Fail-Update $_.Exception.Message
    }
  }
})

$updateButton.Add_Click({
  if ($Update.State -ne 'available') {
    return
  }
  $Update.State = 'downloading'
  $Update.Size = [long]$Update.Zip.size
  $Update.File = Join-Path ([IO.Path]::GetTempPath()) $Update.Zip.name
  Remove-Item -LiteralPath $Update.File -Force -ErrorAction SilentlyContinue
  $Update.Client = New-WebClient
  $Update.Task = $Update.Client.DownloadFileTaskAsync($Update.Zip.browser_download_url, $Update.File)
  $playButton.Enabled = $false
  Show-Banner "Downloading $($Update.Tag)..." $null
  $updateTimer.Start()
})

$form.Add_Shown({
  if ($Version) {
    $Update.State = 'checking'
    $Update.Client = New-WebClient
    $Update.Task = $Update.Client.DownloadStringTaskAsync("https://api.github.com/repos/$Repo/releases/latest")
    $updateTimer.Start()
  }
})

Select-Tab ($Grids.Keys | Select-Object -First 1)
Load-Form
[void]$form.ShowDialog()
