# Window-specific visual shield. No global input hooks; not a keyboard lock.
param([long]$WindowHandle, [int]$OwnerPid, [string]$Signal)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;
public class CodexShield : Form {
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int L,T,R,B; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
    readonly IntPtr target; readonly int owner; readonly string signal;
    protected override bool ShowWithoutActivation { get { return true; } }
    protected override CreateParams CreateParams { get { var p=base.CreateParams; p.ExStyle |= 0x08000000; return p; } }
    protected override void WndProc(ref Message m) { if(m.Msg==0x21){m.Result=new IntPtr(3);return;}base.WndProc(ref m); }
    public CodexShield(long handle,int pid,string path) {
        target=new IntPtr(handle);owner=pid;signal=path;
        FormBorderStyle=FormBorderStyle.None;ShowInTaskbar=false;TopMost=true;
        BackColor=Color.FromArgb(55,55,55);Opacity=.86;
        var title=new Label { Text="Codex is controlling this window", AutoSize=true, ForeColor=Color.White, Font=new Font("Segoe UI",14,FontStyle.Bold) };
        var stop=new Button { Text="Stop takeover", AutoSize=true, BackColor=Color.White };
        Controls.Add(title);Controls.Add(stop);
        Action layout=()=>{ title.Location=new Point(Math.Max(0,(Width-title.Width)/2),Math.Max(0,Height/2-25));stop.Location=new Point(Math.Max(0,(Width-stop.Width)/2),Height/2+15); };
        Resize+=(s,e)=>layout();stop.Click+=(s,e)=>{File.WriteAllText(signal,"stopped");Close();};
        var timer=new Timer { Interval=150 };
        timer.Tick+=(s,e)=>{
            try { System.Diagnostics.Process.GetProcessById(owner); } catch { Close();return; }
            if(File.Exists(signal)||!IsWindow(target)){Close();return;}
            if(!File.Exists(signal+".ready"))File.WriteAllText(signal+".ready","ready");
            Rect r;
            if(GetForegroundWindow()!=target || !GetWindowRect(target,out r)){Hide();return;}
            Bounds=new Rectangle(r.L,r.T,r.R-r.L,r.B-r.T);if(!Visible)Show();
        };
        timer.Start();layout();
    }
}
'@
$form = [CodexShield]::new($WindowHandle, $OwnerPid, $Signal)
# Running an ApplicationContext leaves the shield hidden until the target is foreground.
$context = [System.Windows.Forms.ApplicationContext]::new()
$form.add_FormClosed({ $context.ExitThread() })
[System.Windows.Forms.Application]::Run($context)
