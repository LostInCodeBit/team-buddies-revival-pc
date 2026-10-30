// Quick scan: find BIOS call stubs (li t2,0xA0/B0/C0; jr t2; li t1,N) and syscalls in an image.
using System;
using System.IO;

static class Scan
{
    static int Main(string[] args)
    {
        foreach (var a in args)
        {
            var parts = a.Split('@');
            var d = File.ReadAllBytes(parts[0]);
            uint baseAddr = Convert.ToUInt32(parts[1], 16);
            int off = parts.Length > 2 ? Convert.ToInt32(parts[2], 16) : 0;
            Console.WriteLine("== " + parts[0]);
            for (int i = off; i + 12 <= d.Length; i += 4)
            {
                uint w0 = BitConverter.ToUInt32(d, i), w1 = BitConverter.ToUInt32(d, i + 4), w2 = BitConverter.ToUInt32(d, i + 8);
                uint addr = (uint)(baseAddr + i - off);
                // addiu t2,zero,imm = 0x240A0000|imm ; jr t2 = 0x01400008 ; addiu t1,zero,n = 0x24090000|n
                if ((w0 & 0xFFFF0000) == 0x240A0000 && w1 == 0x01400008 && (w2 & 0xFFFF0000) == 0x24090000)
                    Console.WriteLine("{0:X8}: BIOS {1:X2}:{2:X2}", addr, w0 & 0xFFFF, w2 & 0xFFFF);
                if ((w0 & 0xFC00003F) == 0x0000000C)
                    Console.WriteLine("{0:X8}: syscall (a0 set before?) prev={1:X8}", addr, i >= 4 ? BitConverter.ToUInt32(d, i - 4) : 0);
                // mtc0 / mfc0 / rfe usage
                if ((w0 & 0xFFE00000) == 0x40800000) Console.WriteLine("{0:X8}: mtc0 r{1},cop0r{2}", addr, (w0 >> 16) & 31, (w0 >> 11) & 31);
                if (w0 == 0x42000010) Console.WriteLine("{0:X8}: rfe", addr);
            }
        }
        return 0;
    }
}
