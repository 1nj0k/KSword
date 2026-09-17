# Per-vCPU affinity, memory integrity, scheduler yields, file round trips and loopback UDP.
if (-not ('KswordLabWorkload' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Runtime.InteropServices;
public static class KswordLabWorkload {
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentThread();
    [DllImport("kernel32.dll", SetLastError=true)] static extern UIntPtr SetThreadAffinityMask(IntPtr t, UIntPtr mask);
    [DllImport("kernel32.dll")] static extern uint GetCurrentProcessorNumber();
    static volatile bool stop;
    static Thread[] threads;
    static long[] progress;
    static string[] errors;
    public static long[] Progress() { long[] copy=new long[progress.Length]; for(int i=0;i<copy.Length;i++) copy[i]=Interlocked.Read(ref progress[i]); return copy; }
    public static string[] Errors() { return errors.Where(x => x != null).ToArray(); }
    public static void Start(int count, string directory) {
        if (threads != null) throw new InvalidOperationException("Already running");
        stop=false; progress=new long[count]; errors=new string[count]; threads=new Thread[count];
        for(int i=0;i<count;i++) {
            int cpu=i;
            threads[i]=new Thread(() => Worker(cpu,directory)); threads[i].IsBackground=true; threads[i].Start();
        }
    }
    static void Worker(int cpu,string directory) {
        try {
            if(SetThreadAffinityMask(GetCurrentThread(),new UIntPtr(1UL << cpu)) == UIntPtr.Zero) throw new Exception("Affinity failed");
            byte[] bytes=new byte[1024*1024];
            string path=Path.Combine(directory,"worker-"+cpu+".bin");
            using(var socket=new UdpClient(new IPEndPoint(IPAddress.Loopback,0))) {
                socket.Client.ReceiveTimeout=5000;
                var endpoint=(IPEndPoint)socket.Client.LocalEndPoint;
                while(!stop) {
                    long step=Interlocked.Read(ref progress[cpu]);
                    for(int i=0;i<bytes.Length;i++) bytes[i]=(byte)(i+cpu+step);
                    Thread.Yield();
                    for(int i=0;i<bytes.Length;i++) if(bytes[i]!=(byte)(i+cpu+step)) throw new Exception("Memory mismatch");
                    if(GetCurrentProcessorNumber()!=(uint)cpu) throw new Exception("Affinity drift");
                    if((step & 63)==0) {
                        File.WriteAllBytes(path,bytes);
                        if(!File.ReadAllBytes(path).SequenceEqual(bytes)) throw new Exception("File mismatch");
                        socket.Send(bytes,1024,endpoint);
                        IPEndPoint peer=null; byte[] received=socket.Receive(ref peer);
                        if(!received.SequenceEqual(bytes.Take(1024))) throw new Exception("UDP mismatch");
                    }
                    Interlocked.Increment(ref progress[cpu]);
                }
            }
        } catch(Exception e) { errors[cpu]=e.ToString(); }
    }
    public static void Stop() {
        stop=true;
        foreach(var thread in threads) if(!thread.Join(15000)) throw new Exception("Worker did not stop");
        threads=null;
    }
}
'@
}
