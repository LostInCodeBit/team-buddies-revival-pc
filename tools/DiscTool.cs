// DiscTool: list and extract files from a PS1 MODE2/2352 BIN image (ISO9660).
// Usage: DiscTool list <bin>
//        DiscTool extract <bin> <outdir>
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

static class DiscTool
{
    const int RAW = 2352;

    static FileStream bin;

    static byte[] ReadSector(int lba)
    {
        var raw = new byte[RAW];
        bin.Seek((long)lba * RAW, SeekOrigin.Begin);
        bin.Read(raw, 0, RAW);
        var data = new byte[2048];
        Array.Copy(raw, 24, data, 0, 2048);
        return data;
    }

    static byte[] ReadExtent(int lba, int size)
    {
        var o = new byte[size];
        int off = 0;
        while (off < size)
        {
            var s = ReadSector(lba++);
            int n = Math.Min(2048, size - off);
            Array.Copy(s, 0, o, off, n);
            off += n;
        }
        return o;
    }

    class Entry { public string Path; public int Lba; public int Size; public bool Dir; public byte[] Sys; }

    static int U32(byte[] b, int o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24); }

    static void Walk(int lba, int size, string prefix, List<Entry> list, HashSet<int> seen)
    {
        if (!seen.Add(lba)) return;
        var d = ReadExtent(lba, size);
        int p = 0;
        while (p < d.Length)
        {
            int len = d[p];
            if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }
            int elba = U32(d, p + 2);
            int esize = U32(d, p + 10);
            int flags = d[p + 25];
            int nlen = d[p + 32];
            string name = Encoding.ASCII.GetString(d, p + 33, nlen);
            int sysOff = 33 + nlen + ((nlen & 1) == 0 ? 1 : 0);
            byte[] sys = null;
            if (sysOff < len) { sys = new byte[len - sysOff]; Array.Copy(d, p + sysOff, sys, 0, sys.Length); }
            if (!(nlen == 1 && (d[p + 33] == 0 || d[p + 33] == 1)))
            {
                var e = new Entry { Path = prefix + "/" + name, Lba = elba, Size = esize, Dir = (flags & 2) != 0, Sys = sys };
                list.Add(e);
                if (e.Dir) Walk(elba, esize, e.Path, list, seen);
            }
            p += len;
        }
    }

    static int Main(string[] args)
    {
        bin = File.OpenRead(args[1]);
        var pvd = ReadSector(16);
        int rootLba = U32(pvd, 156 + 2), rootSize = U32(pvd, 156 + 10);
        Console.WriteLine("Volume: " + Encoding.ASCII.GetString(pvd, 40, 32).Trim() + "  sectors=" + (bin.Length / RAW));
        var list = new List<Entry>();
        Walk(rootLba, rootSize, "", list, new HashSet<int>());
        if (args[0] == "list")
        {
            foreach (var e in list)
            {
                string xa = "";
                if (e.Sys != null && e.Sys.Length >= 14 && e.Sys[6] == 'X' && e.Sys[7] == 'A')
                {
                    int attr = (e.Sys[4] << 8) | e.Sys[5];
                    xa = string.Format(" XA attr={0:X4}{1}{2}", attr, (attr & 0x4000) != 0 ? " INTERLEAVED" : "", (attr & 0x1000) != 0 ? " FORM2" : "");
                }
                Console.WriteLine("{0,-40} lba={1,7} size={2,10}{3}{4}", e.Path, e.Lba, e.Size, e.Dir ? " <DIR>" : "", xa);
            }
        }
        else if (args[0] == "extract")
        {
            string outDir = args[2];
            foreach (var e in list)
            {
                string path = Path.Combine(outDir, e.Path.TrimStart('/').Replace(";1", ""));
                if (e.Dir) { Directory.CreateDirectory(path); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                bool form2 = e.Sys != null && e.Sys.Length >= 14 && e.Sys[6] == 'X' && (((e.Sys[4] << 8) | e.Sys[5]) & 0x5000) != 0;
                if (form2)
                {
                    // Raw-copy interleaved/Form2 files (STR/XA) as 2352-byte sectors.
                    int n = (e.Size + 2047) / 2048;
                    using (var f = File.Create(path + ".raw"))
                    {
                        var raw = new byte[RAW];
                        for (int i = 0; i < n; i++) { bin.Seek((long)(e.Lba + i) * RAW, SeekOrigin.Begin); bin.Read(raw, 0, RAW); f.Write(raw, 0, RAW); }
                    }
                }
                else File.WriteAllBytes(path, ReadExtent(e.Lba, e.Size));
            }
        }
        return 0;
    }
}
