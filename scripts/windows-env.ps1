# Notify Explorer that persistent PATH changed. Existing terminal processes may
# retain their environment until fully restarted; no execution policy is changed.
function Send-NeroEnvironmentChange {
    try {
        if (-not ('Nero.EnvironmentBroadcast' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Nero {
    public static class EnvironmentBroadcast {
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SendMessageTimeout(IntPtr window, uint message,
            UIntPtr parameter, string text, uint flags, uint timeout, out UIntPtr result);
        public static void Notify() {
            UIntPtr result;
            SendMessageTimeout(new IntPtr(0xffff), 0x001a, UIntPtr.Zero,
                "Environment", 2, 5000, out result);
        }
    }
}
'@
        }
        [Nero.EnvironmentBroadcast]::Notify()
    } catch {
        Write-Warning 'PATH was saved, but notification failed. Sign out and back in if a new terminal still cannot find nero.'
    }
}
