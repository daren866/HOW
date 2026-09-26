# gui_verify.ps1 - GUI 渲染实证（真桌面）：--show 打开兼容层 -> PrintWindow 截图 -> 像素签名
param(
  [string]$Exe = ".\HOW.exe",
  [string]$Bundle = "com.example.hello"
)
$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing

Write-Host "== apps 目录核查 =="
Get-ChildItem "$env:APPDATA\HOW\apps" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 12 -ExpandProperty FullName

$p = Start-Process -FilePath $Exe -ArgumentList "--show", $Bundle -PassThru
Write-Host ("HOW.exe PID = {0}" -f $p.Id)
$exitCode = 0
try {
  Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class HW {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
  public struct R { public int L,T,Rt,B; }
}
"@
  # 轮询最多 ~12s 等兼容层窗口出现（PID+类名枚举查找，比 FindWindow 可靠：
  # PS 将 $null 标题参数封送为空串会使 FindWindow 匹配不到非空标题窗口）
  $script:targetPid = $p.Id
  $h = [IntPtr]::Zero
  for ($i = 0; $i -lt 12 -and $h -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Seconds 1
    $script:foundHwnd = [IntPtr]::Zero
    $finder = {
      param($wh, $l)
      $wpid = 0
      [HW]::GetWindowThreadProcessId($wh, [ref]$wpid) | Out-Null
      if ($wpid -eq $script:targetPid) {
        $c = New-Object System.Text.StringBuilder 256
        [HW]::GetClassName($wh, $c, 256) | Out-Null
        if ($c.ToString() -eq "HOW_COMPAT_WND") { $script:foundHwnd = $wh; return $false }
      }
      return $true
    }
    [HW]::EnumWindows($finder, [IntPtr]::Zero) | Out-Null
    $h = $script:foundHwnd
    if ($h -ne [IntPtr]::Zero -and -not [HW]::IsWindow($h)) { $h = [IntPtr]::Zero }
    if ($h -ne [IntPtr]::Zero) { break }
    if ($i -eq 5) {
      # 进程还活着吗？
      $alive = $false
      try { $alive = -not (Get-Process -Id $p.Id -ErrorAction Stop).HasExited } catch {}
      Write-Host ("[diag] t+6s 进程存活: {0}" -f $alive)
      # 枚举该进程全部顶层窗口（类名/标题/可见性）
      $cb = {
        param($wh, $l)
        $wpid = 0
        [HW]::GetWindowThreadProcessId($wh, [ref]$wpid) | Out-Null
        if ($wpid -eq $p.Id) {
          $t = New-Object System.Text.StringBuilder 256
          $c = New-Object System.Text.StringBuilder 256
          [HW]::GetWindowText($wh, $t, 256) | Out-Null
          [HW]::GetClassName($wh, $c, 256) | Out-Null
          Write-Host ("[diag] 窗口: class={0} title={1} visible={2}" -f $c.ToString(), $t.ToString(), [HW]::IsWindowVisible($wh))
        }
        return $true
      }
      [HW]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
      # 检测 MessageBox（#32770）并抽取正文
      $mb = {
        param($wh, $l)
        $wpid = 0
        [HW]::GetWindowThreadProcessId($wh, [ref]$wpid) | Out-Null
        if ($wpid -eq $p.Id) {
          $c = New-Object System.Text.StringBuilder 256
          [HW]::GetClassName($wh, $c, 256) | Out-Null
          if ($c.ToString() -eq "#32770") {
            $t = New-Object System.Text.StringBuilder 256
            [HW]::GetWindowText($wh, $t, 256) | Out-Null
            Write-Host ("[diag] MessageBox 标题: {0}" -f $t.ToString())
            $cb2 = {
              param($ch, $l2)
              $st = New-Object System.Text.StringBuilder 512
              [HW]::GetWindowText($ch, $st, 512) | Out-Null
              if ($st.Length -gt 0) { Write-Host ("[diag] MessageBox 正文: {0}" -f $st.ToString()) }
              return $true
            }
            [HW]::EnumChildWindows($wh, $cb2, [IntPtr]::Zero) | Out-Null
          }
        }
        return $true
      }
      [HW]::EnumWindows($mb, [IntPtr]::Zero) | Out-Null
    }
  }
  if ($h -eq [IntPtr]::Zero -or -not [HW]::IsWindow($h)) {
    Write-Host "FAIL: 兼容层窗口未找到（--show 启动失败）"
    exit 1
  }
  Write-Host ("兼容层窗口已打开: 0x{0:X}" -f $h.ToInt64())
  $wr = New-Object HW+R
  [HW]::GetWindowRect($h, [ref]$wr) | Out-Null
  $ww = $wr.Rt - $wr.L; $wh2 = $wr.B - $wr.T
  Write-Host "窗口尺寸: ${ww}x${wh2}"
  $bmp = New-Object System.Drawing.Bitmap($ww, $wh2)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $okp = [HW]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
  $g.ReleaseHdc($hdc); $g.Dispose()
  Write-Host "PrintWindow(PW_RENDERFULLCONTENT): $okp"
  $bmp.Save("$PWD\compat-window.png", [System.Drawing.Imaging.ImageFormat]::Png)

  # 像素签名软分析（截图受会话状态影响，只报告；硬门禁在 selftest 像素验证）
  $cnt = @{ toolbar = 0; page = 0; blue = 0; red = 0; log = 0; black = 0 }
  for ($y = 0; $y -lt $wh2; $y += 3) {
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
  $total = [Math]::Ceiling($ww / 3.0) * [Math]::Ceiling($wh2 / 3.0)
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
