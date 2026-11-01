$ErrorActionPreference = 'Stop'

# Set only the two stereo playback endpoints KMX6 uses for its computer
# channels. The 16-channel compatibility endpoint is intentionally untouched.
$source = @'
using System;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)]
public struct KMX6PropertyKey { public Guid FormatId; public uint PropertyId; }

[StructLayout(LayoutKind.Explicit, Size = 24)]
public struct KMX6PropVariant
{
    [FieldOffset(0)] public ushort Type;
    [FieldOffset(8)] public IntPtr Value;
}

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface KMX6IMMDeviceEnumerator
{
    [PreserveSig] int EnumAudioEndpoints(int flow, uint stateMask, out KMX6IMMDeviceCollection devices);
}

[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface KMX6IMMDeviceCollection
{
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int Item(uint index, out KMX6IMMDevice device);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface KMX6IMMDevice
{
    [PreserveSig] int Activate(ref Guid iid, uint context, IntPtr parameters, out IntPtr instance);
    [PreserveSig] int OpenPropertyStore(uint access, out KMX6IPropertyStore properties);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    [PreserveSig] int GetState(out uint state);
}

[ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface KMX6IPropertyStore
{
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int GetAt(uint index, out KMX6PropertyKey key);
    [PreserveSig] int GetValue(ref KMX6PropertyKey key, out KMX6PropVariant value);
    [PreserveSig] int SetValue(ref KMX6PropertyKey key, ref KMX6PropVariant value);
    [PreserveSig] int Commit();
}

[ComImport, Guid("F8679F50-850A-41CF-9C72-430F290290C8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface KMX6IPolicyConfig
{
    [PreserveSig] int GetMixFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr format);
    [PreserveSig] int GetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, [MarshalAs(UnmanagedType.Bool)] bool defaultFormat, IntPtr format);
    [PreserveSig] int ResetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id);
    [PreserveSig] int SetDeviceFormat([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr endpointFormat, IntPtr mixFormat);
    [PreserveSig] int GetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, [MarshalAs(UnmanagedType.Bool)] bool defaultPeriod, IntPtr defaultPeriodOut, IntPtr minimumPeriodOut);
    [PreserveSig] int SetProcessingPeriod([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr period);
    [PreserveSig] int GetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr mode);
    [PreserveSig] int SetShareMode([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr mode);
    [PreserveSig] int GetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr key, IntPtr value);
    [PreserveSig] int SetPropertyValue([MarshalAs(UnmanagedType.LPWStr)] string id, ref KMX6PropertyKey key, ref KMX6PropVariant value);
}

[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
public class KMX6MMDeviceEnumerator { }

[ComImport, Guid("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9")]
public class KMX6PolicyConfig { }

public static class KMX6EndpointRenamer
{
    static readonly Guid DeviceDesc = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0");
    static readonly Guid FriendlyName = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0");

    static string ReadString(KMX6IPropertyStore store, uint propertyId)
    {
        var key = new KMX6PropertyKey { FormatId = DeviceDesc, PropertyId = propertyId };
        KMX6PropVariant value;
        if (store.GetValue(ref key, out value) != 0 || value.Type != 31) return null;
        return Marshal.PtrToStringUni(value.Value);
    }

    public static string Rename()
    {
        object enumeratorObject = null, policyObject = null;
        KMX6IMMDeviceCollection devices = null;
        var changed = new System.Collections.Generic.List<string>();
        try
        {
            enumeratorObject = new KMX6MMDeviceEnumerator();
            var enumerator = (KMX6IMMDeviceEnumerator)enumeratorObject;
            int enumResult = enumerator.EnumAudioEndpoints(2, 1, out devices);
            if (enumResult != 0) return "Could not enumerate playback endpoints: 0x" + enumResult.ToString("X8");
            policyObject = new KMX6PolicyConfig();
            var policy = (KMX6IPolicyConfig)policyObject;
            var friendlyKey = new KMX6PropertyKey { FormatId = FriendlyName, PropertyId = 14 };
            uint count;
            int countResult = devices.GetCount(out count);
            if (countResult != 0) return "Could not read Windows playback endpoints.";
            for (uint i = 0; i < count; i++)
            {
                KMX6IMMDevice device;
                int itemResult = devices.Item(i, out device);
                if (itemResult != 0) continue;
                KMX6IPropertyStore store = null;
                try
                {
                    string id;
                    int idResult = device.GetId(out id);
                    int storeResult = idResult == 0 ? device.OpenPropertyStore(0, out store) : idResult;
                    if (idResult != 0 || storeResult != 0) continue;
                    string desc = ReadString(store, 2);
                    string current = ReadString(store, 14);
                    string target = null;
                    var match = System.Text.RegularExpressions.Regex.Match(current ?? "", @"\(.+ (Computer [12]|Mic B[1-4])\)$"); if (match.Success) target = "KMX6 " + match.Groups[1].Value;
                    
                    if (target == null || current == target || current.Contains("KMX6 ")) continue;

                    var value = new KMX6PropVariant { Type = 31, Value = Marshal.StringToCoTaskMemUni(target) };
                    try
                    {
                        int result = policy.SetPropertyValue(id, ref friendlyKey, ref value);
                        if (result != 0) { KMX6IPropertyStore writable = null; try { if (device.OpenPropertyStore(2, out writable) == 0) { result = writable.SetValue(ref friendlyKey, ref value); if (result == 0) result = writable.Commit(); } } finally { if (writable != null) Marshal.ReleaseComObject(writable); } }
                        if (result != 0) throw new COMException("Windows could not rename " + desc + " (0x" + result.ToString("X8") + ").", result);
                    }
                    finally { Marshal.FreeCoTaskMem(value.Value); }
                    changed.Add(target);
                }
                finally
                {
                    if (store != null) Marshal.ReleaseComObject(store);
                    Marshal.ReleaseComObject(device);
                }
            }
            return changed.Count == 0 ? "KMX6 playback names are already set or the cable endpoints are not installed." : "Renamed: " + String.Join(", ", changed);
        }
        finally
        {
            if (devices != null) Marshal.ReleaseComObject(devices);
            if (policyObject != null) Marshal.ReleaseComObject(policyObject);
            if (enumeratorObject != null) Marshal.ReleaseComObject(enumeratorObject);
        }
    }
}
'@

Add-Type -TypeDefinition $source -Language CSharp
[KMX6EndpointRenamer]::Rename()
