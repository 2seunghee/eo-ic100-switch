<#
.SYNOPSIS
  Step 1: check the EO-IC100 on Windows and dump its USB descriptors without touching drivers.

.DESCRIPTION
  1. Lists present PnP devices for the vendor ID (default 04E8 = Samsung) with status/problem codes,
     plus matching Windows audio endpoints.
  2. Reads the device, configuration and string descriptors from the parent hub
     (IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, the same way USBView does). The device keeps
     its usbaudio.sys driver; nothing is installed or replaced.
  3. Writes <vid>_<pid>_device.bin / _config.bin / _strings.txt into -OutDir and runs
     pc곞sc.py on the configuration descriptor if Python is available.

  -Watch N polls every N seconds and prints connect/disconnect events, which shows a loose
  USB-C plug (the device dropping off the bus while you wiggle the cable).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File pc\dump_usb_descriptors.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File pc\dump_usb_descriptors.ps1 -All
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File pc\dump_usb_descriptors.ps1 -VendorId 0x0bda -ProductId 0x4bd1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File pc\dump_usb_descriptors.ps1 -Watch 1
#>
[CmdletBinding()]
param(
    [string]$VendorId = "04E8",
    [string]$ProductId = "",
    [switch]$All,
    [string]$OutDir = "",
    [switch]$KeepSerial,
    [switch]$NoAnalyze,
    [int]$Watch = 0
)

$ErrorActionPreference = "Stop"
if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent $PSScriptRoot) "dumps" }

function ConvertFrom-HexId([string]$s) {
    return [Convert]::ToInt32(($s -replace '^0[xX]', ''), 16)
}

$vid = ConvertFrom-HexId $VendorId
$prodId = if ($ProductId) { ConvertFrom-HexId $ProductId } else { -1 }

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class UsbHubDump
{
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_WRITE = 0x2;
    const uint OPEN_EXISTING = 3;
    const uint DIGCF_PRESENT = 0x2;
    const uint DIGCF_DEVICEINTERFACE = 0x10;
    const uint IOCTL_USB_GET_NODE_INFORMATION = 0x220408;
    const uint IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION = 0x220410;
    const uint IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME = 0x220420;
    const uint IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX = 0x220448;
    static readonly Guid GUID_DEVINTERFACE_USB_HUB = new Guid("f18a0e88-c30c-11d0-8815-00a0c906bed8");

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved; }

    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator, IntPtr hwndParent, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr devInfo, ref Guid classGuid, uint index, ref SP_DEVICE_INTERFACE_DATA data);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr set, ref SP_DEVICE_INTERFACE_DATA data, IntPtr detail, uint detailSize, out uint requiredSize, IntPtr devInfo);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle dev, uint code, byte[] inBuf, int inSize, byte[] outBuf, int outSize, out int returned, IntPtr overlapped);

    public class Device
    {
        public string HubPath;
        public int Port;
        public int Speed;          // 0 low, 1 full, 2 high, 3 super
        public int Address;
        public int ConnectionStatus;
        public byte[] DeviceDescriptor;
        public byte[] ConfigDescriptor;
        public string ConfigError;
        public string DriverKey;
        public Dictionary<int, string> Strings = new Dictionary<int, string>();
        public Dictionary<int, byte[]> HidReports = new Dictionary<int, byte[]>();   // interface -> report descriptor
        public Dictionary<int, string> HidErrors = new Dictionary<int, string>();
        public int Vid { get { return DeviceDescriptor[8] | (DeviceDescriptor[9] << 8); } }
        public int Pid { get { return DeviceDescriptor[10] | (DeviceDescriptor[11] << 8); } }
    }

    public static List<string> HubPaths()
    {
        List<string> list = new List<string>();
        Guid g = GUID_DEVINTERFACE_USB_HUB;
        IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == new IntPtr(-1)) throw new Win32Exception();
        try
        {
            for (uint i = 0; ; i++)
            {
                SP_DEVICE_INTERFACE_DATA did = new SP_DEVICE_INTERFACE_DATA();
                did.cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref did)) break;
                uint need;
                SetupDiGetDeviceInterfaceDetail(set, ref did, IntPtr.Zero, 0, out need, IntPtr.Zero);
                if (need == 0) continue;
                IntPtr buf = Marshal.AllocHGlobal((int)need);
                try
                {
                    // SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize: 8 on x64, 6 on x86; the path starts at +4.
                    Marshal.WriteInt32(buf, IntPtr.Size == 8 ? 8 : 6);
                    if (SetupDiGetDeviceInterfaceDetail(set, ref did, buf, need, out need, IntPtr.Zero))
                        list.Add(Marshal.PtrToStringUni(new IntPtr(buf.ToInt64() + 4)));
                }
                finally { Marshal.FreeHGlobal(buf); }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
        return list;
    }

    static bool Ioctl(SafeFileHandle h, uint code, byte[] buf, out int returned)
    {
        return DeviceIoControl(h, code, buf, buf.Length, buf, buf.Length, out returned, IntPtr.Zero);
    }

    static int PortCount(SafeFileHandle h)
    {
        // USB_NODE_INFORMATION: USB_HUB_NODE NodeType (4) + USB_HUB_DESCRIPTOR (bLength, bType, bNumberOfPorts, ...)
        byte[] buf = new byte[256];
        int ret;
        if (!Ioctl(h, IOCTL_USB_GET_NODE_INFORMATION, buf, out ret) || ret < 7) return 0;
        if (BitConverter.ToInt32(buf, 0) != 0) return 0; // not UsbHub
        return buf[6];
    }

    // USB_DESCRIPTOR_REQUEST (packed): ULONG ConnectionIndex; 8-byte setup packet; UCHAR Data[]
    public static byte[] GetDescriptor(SafeFileHandle h, int port, int type, int index, int langId, int length)
    {
        return GetDescriptor(h, port, 0x80, type, index, langId, length);
    }

    // bmRequest 0x81 (interface recipient) is needed for HID report descriptors; wIndex = interface number.
    public static byte[] GetDescriptor(SafeFileHandle h, int port, int bmRequest, int type, int index, int wIndex, int length)
    {
        const int hdr = 12;
        byte[] buf = new byte[hdr + length];
        BitConverter.GetBytes(port).CopyTo(buf, 0);
        buf[4] = (byte)bmRequest;
        buf[5] = 0x06;
        BitConverter.GetBytes((ushort)((type << 8) | index)).CopyTo(buf, 6);
        BitConverter.GetBytes((ushort)wIndex).CopyTo(buf, 8);
        BitConverter.GetBytes((ushort)length).CopyTo(buf, 10);
        int ret;
        if (!Ioctl(h, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buf, out ret)) return null;
        int n = ret - hdr;
        if (n <= 0) return new byte[0];
        byte[] data = new byte[n];
        Array.Copy(buf, hdr, data, 0, n);
        return data;
    }

    // (interface number, report descriptor length) for every HID interface (alt 0) in a config descriptor.
    static List<int[]> HidInterfaces(byte[] cfg)
    {
        List<int[]> list = new List<int[]>();
        int pos = cfg.Length >= 9 ? cfg[0] : cfg.Length, curIf = -1, curAlt = -1;
        while (pos + 2 <= cfg.Length)
        {
            int len = cfg[pos], type = cfg[pos + 1];
            if (len < 2 || pos + len > cfg.Length) break;
            if (type == 0x04 && len >= 9) { curIf = cfg[pos + 2]; curAlt = cfg[pos + 3]; }
            else if (type == 0x21 && len >= 9 && curAlt == 0) list.Add(new int[] { curIf, cfg[pos + 7] | (cfg[pos + 8] << 8) });
            pos += len;
        }
        return list;
    }

    static string DriverKey(SafeFileHandle h, int port)
    {
        // USB_NODE_CONNECTION_DRIVERKEY_NAME: ULONG ConnectionIndex; ULONG ActualLength; WCHAR DriverKeyName[]
        byte[] buf = new byte[1024];
        BitConverter.GetBytes(port).CopyTo(buf, 0);
        int ret;
        if (!Ioctl(h, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, buf, out ret) || ret <= 8) return null;
        return Encoding.Unicode.GetString(buf, 8, ((ret - 8) / 2) * 2).TrimEnd('\0');
    }

    public static List<Device> Scan(int vid, int pid, bool all, bool readDescriptors)
    {
        List<Device> result = new List<Device>();
        foreach (string hub in HubPaths())
        {
            using (SafeFileHandle h = CreateFile(hub, GENERIC_WRITE, FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero))
            {
                if (h.IsInvalid) continue;
                int ports = PortCount(h);
                for (int port = 1; port <= ports; port++)
                {
                    // USB_NODE_CONNECTION_INFORMATION_EX (packed): ConnectionIndex@0, DeviceDescriptor@4 (18),
                    // CurrentConfigurationValue@22, Speed@23, DeviceIsHub@24, DeviceAddress@25, NumberOfOpenPipes@27,
                    // ConnectionStatus@31, PipeList@35
                    byte[] ci = new byte[1024];
                    BitConverter.GetBytes(port).CopyTo(ci, 0);
                    int ret;
                    if (!Ioctl(h, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, ci, out ret) || ret < 35) continue;
                    if (ci[4] != 18 || ci[5] != 1 || ci[24] != 0) continue; // no device, or a hub
                    Device d = new Device();
                    d.HubPath = hub;
                    d.Port = port;
                    d.Speed = ci[23];
                    d.Address = BitConverter.ToUInt16(ci, 25);
                    d.ConnectionStatus = BitConverter.ToInt32(ci, 31);
                    d.DeviceDescriptor = new byte[18];
                    Array.Copy(ci, 4, d.DeviceDescriptor, 0, 18);
                    if (!all && (d.Vid != vid || (pid >= 0 && d.Pid != pid))) continue;
                    if (readDescriptors)
                    {
                        byte[] head = GetDescriptor(h, port, 2, 0, 0, 9);
                        if (head == null || head.Length < 4) d.ConfigError = "GET_DESCRIPTOR(CONFIG,9) failed: Win32 error " + Marshal.GetLastWin32Error();
                        else
                        {
                            int total = head[2] | (head[3] << 8);
                            d.ConfigDescriptor = GetDescriptor(h, port, 2, 0, 0, total);
                            if (d.ConfigDescriptor == null) d.ConfigError = "GET_DESCRIPTOR(CONFIG," + total + ") failed: Win32 error " + Marshal.GetLastWin32Error();
                        }
                        if (d.ConfigDescriptor != null)
                        {
                            foreach (int[] hid in HidInterfaces(d.ConfigDescriptor))
                            {
                                byte[] rep = GetDescriptor(h, port, 0x81, 0x22, 0, hid[0], hid[1]);
                                if (rep != null && rep.Length > 0) d.HidReports[hid[0]] = rep;
                                else d.HidErrors[hid[0]] = rep == null ? "Win32 error " + Marshal.GetLastWin32Error() : "empty reply";
                            }
                        }
                        byte[] langs = GetDescriptor(h, port, 3, 0, 0, 255);
                        int lang = (langs != null && langs.Length >= 4) ? (langs[2] | (langs[3] << 8)) : 0x0409;
                        for (int si = 1; si <= 15; si++)
                        {
                            byte[] s = GetDescriptor(h, port, 3, si, lang, 255);
                            if (s != null && s.Length >= 2 && s[1] == 3)
                                d.Strings[si] = Encoding.Unicode.GetString(s, 2, ((Math.Min(s[0], s.Length) - 2) / 2) * 2);
                        }
                        d.DriverKey = DriverKey(h, port);
                    }
                    result.Add(d);
                }
            }
        }
        return result;
    }
}
"@

$speedNames = @{ 0 = "low-speed"; 1 = "full-speed (12 Mbps)"; 2 = "high-speed (480 Mbps)"; 3 = "super-speed" }

function Show-PnpState {
    $pat = 'VID_{0:X4}' -f $vid
    if ($prodId -ge 0) { $pat += ('&PID_{0:X4}' -f $prodId) }
    Write-Host "== [1/3] PnP devices matching $pat ==" -ForegroundColor Cyan
    $pnp = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match [regex]::Escape($pat) -or ($All -and $_.InstanceId -match '^USB\\VID_') })
    if ($pnp.Count -eq 0) {
        Write-Host "  (none) - the device is not enumerated by Windows. Re-seat the plug, try another port/cable." -ForegroundColor Yellow
    }
    foreach ($d in $pnp) {
        $problem = if ($d.ConfigManagerErrorCode -ne 0) { "  <-- ConfigManagerErrorCode $($d.ConfigManagerErrorCode)" } else { "" }
        $id = $d.InstanceId
        # USB\VID_x&PID_y\<instance>: Windows uses the device's serial number as the instance part
        # when it has one (no '&' in it); generated instance IDs always contain '&'.
        $parts = $id.Split('\')
        if (-not $KeepSerial -and $parts.Count -ge 3 -and $parts[0] -eq 'USB' -and $parts[2] -notmatch '&') {
            $parts[2] = '<serial>'
            $id = $parts -join '\'
        }
        Write-Host ("  {0,-8} {1,-12} {2,-40} {3}{4}" -f $d.Status, $d.Class, $d.FriendlyName, $id, $problem)
    }
    $ep = @(Get-PnpDevice -Class AudioEndpoint -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match 'Samsung|Earphone|USB' })
    foreach ($e in $ep) { Write-Host ("  audio endpoint: {0,-8} {1}" -f $e.Status, $e.FriendlyName) }
}

function Save-Device($d) {
    $tag = '{0:x4}_{1:x4}' -f $d.Vid, $d.Pid
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $OutDir "${tag}_device.bin"), $d.DeviceDescriptor)
    $cfgPath = $null
    if ($d.ConfigDescriptor) {
        $cfgPath = Join-Path $OutDir "${tag}_config.bin"
        [IO.File]::WriteAllBytes($cfgPath, $d.ConfigDescriptor)
    }
    $dd = $d.DeviceDescriptor
    $lines = @(
        "vid:pid      $tag",
        ("speed        {0}" -f $speedNames[[int]$d.Speed]),
        "hub          $($d.HubPath)",
        "port         $($d.Port)  address $($d.Address)  driver key $($d.DriverKey)"
    )
    foreach ($k in ($d.Strings.Keys | Sort-Object)) {
        $v = $d.Strings[$k]
        if ($k -eq $dd[16] -and -not $KeepSerial) { $v = "<serial redacted, $($v.Length) chars; use -KeepSerial>" }
        $lines += ("string[{0,2}]   {1}" -f $k, $v)
    }
    foreach ($k in ($d.HidReports.Keys | Sort-Object)) {
        $path = Join-Path $OutDir ("{0}_hid_if{1}.bin" -f $tag, $k)
        [IO.File]::WriteAllBytes($path, $d.HidReports[$k])
        $lines += ("hid IF{0}       report descriptor {1} bytes -> {2}" -f $k, $d.HidReports[$k].Length, (Split-Path -Leaf $path))
    }
    foreach ($k in ($d.HidErrors.Keys | Sort-Object)) {
        $lines += ("hid IF{0}       report descriptor not readable via the hub ({1})" -f $k, $d.HidErrors[$k])
    }
    $lines | Set-Content -Encoding UTF8 -Path (Join-Path $OutDir "${tag}_strings.txt")
    return @{ Tag = $tag; ConfigPath = $cfgPath; Lines = $lines }
}

function Invoke-Dump {
    Write-Host "== [2/3] descriptors from the parent hub (no driver change) ==" -ForegroundColor Cyan
    $devs = [UsbHubDump]::Scan($vid, $prodId, [bool]$All, $true)
    if ($devs.Count -eq 0) {
        Write-Host ("  no matching device on any USB hub (vendor 0x{0:x4})" -f $vid) -ForegroundColor Yellow
        return
    }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
    $analyzer = Join-Path $PSScriptRoot "uacdesc.py"
    foreach ($d in $devs) {
        $saved = Save-Device $d
        Write-Host ""
        foreach ($l in $saved.Lines) { Write-Host "  $l" }
        if ($d.ConfigError) { Write-Host "  $($d.ConfigError)" -ForegroundColor Yellow }
        if ($saved.ConfigPath) {
            Write-Host ("  config descriptor: {0} bytes -> {1}" -f $d.ConfigDescriptor.Length, $saved.ConfigPath)
            if (-not $NoAnalyze -and $python) {
                Write-Host "== [3/3] uacdesc.py ==" -ForegroundColor Cyan
                & $python.Source -I $analyzer $saved.ConfigPath
            }
        }
    }
    if (-not $python -and -not $NoAnalyze) { Write-Host "  (Python not found: run pc곞sc.py on the .bin files later)" }
}

if ($Watch -gt 0) {
    Write-Host ("Watching vendor 0x{0:x4} every {1}s. Wiggle the plug; Ctrl+C to stop." -f $vid, $Watch) -ForegroundColor Cyan
    $prev = @{}
    while ($true) {
        $now = @{}
        foreach ($d in [UsbHubDump]::Scan($vid, $prodId, [bool]$All, $false)) {
            $now[('{0:x4}:{1:x4} port {2} {3}' -f $d.Vid, $d.Pid, $d.Port, $speedNames[[int]$d.Speed])] = $true
        }
        $stamp = Get-Date -Format "HH:mm:ss.fff"
        foreach ($k in $now.Keys) { if (-not $prev.ContainsKey($k)) { Write-Host "$stamp  CONNECTED     $k" -ForegroundColor Green } }
        foreach ($k in $prev.Keys) { if (-not $now.ContainsKey($k)) { Write-Host "$stamp  DISCONNECTED  $k" -ForegroundColor Red } }
        $prev = $now
        Start-Sleep -Seconds $Watch
    }
}

Show-PnpState
Write-Host ""
Invoke-Dump
exit 0  # don't leak uacdesc.py's "RISK found" exit status as a script failure
