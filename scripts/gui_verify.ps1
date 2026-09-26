# gui_verify.ps1 - GUI 渲染实证（真桌面）：--show 打开兼容层 -> PrintWindow 截图 -> 像素签名
param(
  [string]$Exe = ".\HOW.exe",
  [string]$Bundle = "com.example.hello"
)
$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing

$p = Start-Process -FilePath $Exe -ArgumentList "--show", $Bundle -PassThru
$exitCode = 0
try {
  Start-Sleep -Seconds 6
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class HW {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public struct R { public int L,T,Rt,B; }
}
"@
  $h = [HW]::FindWindowW("HOW_COMPAT_WND", $null)
  if ($h -eq [IntPtr]::Zero -or -not [HW]::IsWindow($h)) {
    Write-Host "FAIL: 兼容层窗口未找到（--show 启动失败）"
    exit 1
  }
  Write-Host ("兼容层窗口已打开: 0x{0:X}" -f $h.ToInt64())
  $wr = New-Object HW+R
  [HW]::GetWindowRect($h, [ref]$wr) | Out-Null
  $ww = $wr.Rt - $wr.L; $wh = $wr.B - $wr.T
  Write-Host "窗口尺寸: ${ww}x${wh}"
  $bmp = New-Object System.Drawing.Bitmap($ww, $wh)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $okp = [HW]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
  $g.ReleaseHdc($hdc); $g.Dispose()
  Write-Host "PrintWindow(PW_RENDERFULLCONTENT): $okp"
  $bmp.Save("$PWD\compat-window.png", [System.Drawing.Imaging.ImageFormat]::Png)

  # 像素签名软分析（截图受会话状态影响，只报告；硬门禁在 selftest 像素验证）
  $cnt = @{ toolbar = 0; page = 0; blue = 0; red = 0; log = 0; black = 0 }
  for ($y = 0; $y -lt $wh; $y += 3) {
    for ($x = 0; $x -lt $ww; $x += 3) {
      $c = $bmp.GetPixel($x, $y)
      if     ([Math]::Abs($c.R - 0x18) -le 30 -and [Math]::Abs($c.G - 0x24) -le 30 -and [Math]::Abs($c.B - 0x31) -le 30) { $cnt.toolbar++ }
      elseif ($c.R -ge 0xD0 -and $c.G -ge 0xD0 -and $c.B -ge 0xD0) { $cnt.page++ }
      elseif ($c.R -le 0x40 -and [Math]::Abs($c.G - 0x7D) -le 40 -and [Math]::Abs($c.B - 0xFF) -le 40) { $cnt.blue++ }
      elseif ([Math]::Abs($c.R - 0xE8) -le 40 -and [Math]::Abs($c.G - 0x40) -le 40 -and [Math]::Abs($c.B - 0x26) -le 40) { $cnt.red++ }
      elseif ([Math]::Abs($c.R - 0x0C) -le 25 -and [Math]::Abs($c.G - 0x11) -le 25 -and [Math]::Abs($c.B - 0x16) -le 25) { $cnt.log++ }
      elseif ($c.R -eq 0 -and $c.G -eq 0 -and $c.B -eq 0) { $cnt.black++ }
    }
  }
  $bmp.Dispose()
  $total = [Math]::Ceiling($ww / 3.0) * [Math]::Ceiling($wh / 3.0)
  Write-Host ("像素签名: toolbar={0} page={1} blue={2} red={3} log={4} pureBlack={5}/{6}" -f $cnt.toolbar, $cnt.page, $cnt.blue, $cnt.red, $cnt.log, $cnt.black, $total)
  if ($cnt.black -gt $total * 0.95) {
    Write-Host "WARN: 截图全黑（会话可能断连）—— 渲染硬门禁以 selftest 像素验证为准"
  } elseif ($cnt.page -gt 0 -and $cnt.toolbar -gt 0) {
    Write-Host "GUI 渲染实证: 真桌面截图命中工具栏/页面签名色 OK"
  } else {
    Write-Host "WARN: 截图签名色未命中（查看 compat-window.png artifact）"
  }
} finally {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
exit $exitCode
