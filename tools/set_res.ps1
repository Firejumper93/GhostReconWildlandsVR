# set_res.ps1: list display modes or switch the primary desktop resolution.
# Usage:
#   powershell -File set_res.ps1            -> list devices and largest modes
#   powershell -File set_res.ps1 3840 2160  -> switch primary to WxH
param([int]$W = 0, [int]$H = 0)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DISPLAY_DEVICE {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
    public int StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public ushort dmSpecVersion;
    public ushort dmDriverVersion;
    public ushort dmSize;
    public ushort dmDriverExtra;
    public uint dmFields;
    public int dmPositionX;
    public int dmPositionY;
    public uint dmDisplayOrientation;
    public uint dmDisplayFixedOutput;
    public short dmColor;
    public short dmDuplex;
    public short dmYResolution;
    public short dmTTOption;
    public short dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public ushort dmLogPixels;
    public uint dmBitsPerPel;
    public uint dmPelsWidth;
    public uint dmPelsHeight;
    public uint dmDisplayFlags;
    public uint dmDisplayFrequency;
    public uint dmICMMethod;
    public uint dmICMIntent;
    public uint dmMediaType;
    public uint dmDitherType;
    public uint dmReserved1;
    public uint dmReserved2;
    public uint dmPanningWidth;
    public uint dmPanningHeight;
}

public class Native {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum,
                                                 ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplaySettings(string lpszDeviceName, int iModeNum,
                                                  ref DEVMODE lpDevMode);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int ChangeDisplaySettingsEx(string lpszDeviceName, ref DEVMODE lpDevMode,
                                                     IntPtr hwnd, uint dwflags, IntPtr lParam);
}
'@

Add-Type -AssemblyName System.Windows.Forms
$primaryName = ([System.Windows.Forms.Screen]::AllScreens |
                Where-Object { $_.Primary }).DeviceName
if (-not $primaryName) { Write-Output "NO PRIMARY DEVICE FOUND"; exit 1 }
Write-Output ("primary device: " + $primaryName)

$dm = New-Object DEVMODE
$dm.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($dm)
[void][Native]::EnumDisplaySettings($primaryName, -1, [ref]$dm)
Write-Output ("current: " + $dm.dmPelsWidth + "x" + $dm.dmPelsHeight + " @ " + $dm.dmDisplayFrequency + "Hz")

if ($W -eq 0) {
    # list mode: show the distinct resolutions, largest first
    $seen = @{}
    $m = 0
    while ([Native]::EnumDisplaySettings($primaryName, $m, [ref]$dm)) {
        $k = "" + $dm.dmPelsWidth + "x" + $dm.dmPelsHeight
        if (-not $seen.ContainsKey($k)) { $seen[$k] = [long]$dm.dmPelsWidth * $dm.dmPelsHeight }
        $m++
    }
    Write-Output "modes (largest first):"
    $seen.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 10 |
        ForEach-Object { Write-Output ("  " + $_.Key) }
    exit 0
}

# switch mode: find WxH at the highest refresh rate available
$best = $null
$m = 0
while ([Native]::EnumDisplaySettings($primaryName, $m, [ref]$dm)) {
    if ($dm.dmPelsWidth -eq $W -and $dm.dmPelsHeight -eq $H) {
        if ($null -eq $best -or $dm.dmDisplayFrequency -gt $best.dmDisplayFrequency) { $best = $dm }
    }
    $m++
}
if ($null -eq $best) { Write-Output ("MODE " + $W + "x" + $H + " NOT FOUND on " + $primaryName); exit 1 }
Write-Output ("switching to " + $best.dmPelsWidth + "x" + $best.dmPelsHeight + " @ " + $best.dmDisplayFrequency + "Hz")
$r = [Native]::ChangeDisplaySettingsEx($primaryName, [ref]$best, [IntPtr]::Zero, 1, [IntPtr]::Zero)  # 1 = CDS_UPDATEREGISTRY
Write-Output ("result: " + $r + " (0 = DISP_CHANGE_SUCCESSFUL)")
