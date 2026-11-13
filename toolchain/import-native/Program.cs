using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.Es;
using LibHac.Tools.Ncm;
using Extractor = LibHac.Tools.FsSystem.FileSystemExtensions;
using Path = System.IO.Path;

// base.nca patch.nca control.nca prod.keys output [workers]; title keys arrive on standard input.
if (args.Length == 4 && args[0] == "--dlc")
    return Dlc.Import(args[1], args[2], args[3]);
if (args.Length is < 5 or > 6)
    return 2;
int workers = 1;
if (args.Length == 6 && (!int.TryParse(args[5], out workers) || workers is < 1 or > 16))
    return 2;
int stage = 3;
try
{
    var keys = KeySet.CreateDefaultKeySet();
    ExternalKeyReader.ReadKeyFile(keys, args[3], null, null, null);
    stage = 4;
    using var input = Console.OpenStandardInput();
    using var tickets = new MemoryStream();
    input.CopyTo(tickets);
    byte[] ticketBytes = tickets.ToArray();
    ExternalKeyReader.ReadTitleKeys(keys, new MemoryStream(ticketBytes, false), null);

    stage = 5;
    using var baseStorage = new LocalStorage(args[0], FileAccess.Read);
    using var patchStorage = new LocalStorage(args[1], FileAccess.Read);
    using var controlStorage = new LocalStorage(args[2], FileAccess.Read);
    var baseNca = new Nca(keys, baseStorage);
    var patch = new Nca(keys, patchStorage);
    var control = new Nca(keys, controlStorage);
    const IntegrityCheckLevel check = IntegrityCheckLevel.ErrorOnInvalid;

    using var exefs = patch.OpenFileSystem(NcaSectionType.Code, check);
    Extractor.Extract(exefs, Path.Combine(args[4], "exefs"), null);
    stage = 6;
    using var metadata = control.OpenFileSystem(NcaSectionType.Data, check);
    Extractor.Extract(metadata, Path.Combine(args[4], "control"), null);
    stage = 7;
    using var romfs = baseNca.OpenFileSystemWithPatch(patch, NcaSectionType.Data, check);
    stage = 9;
    var files = new List<(string Path, long Size)>();
    var folders = new List<string>();
    long bytes = 0;
    foreach (var entry in Extractor.EnumerateEntries(romfs, "/", "*", SearchOptions.RecurseSubdirectories))
    {
        if (entry.Type != DirectoryEntryType.File)
        {
            folders.Add(entry.FullPath);  // (empty folders too, as the sequential extraction did)
            continue;
        }
        files.Add((entry.FullPath, entry.Size));
        bytes = checked(bytes + entry.Size);
    }
    if (files.Count == 0)
        return 7;
    if (new DriveInfo(Path.GetPathRoot(args[4])!).AvailableFreeSpace < bytes + (4L << 30))
        return 8;
    stage = 10;
    string root = Path.GetFullPath(Path.Combine(args[4], "romfs"));
    string Local(string path)
    {
        string target = Path.GetFullPath(Path.Combine(root, path.TrimStart('/').Replace('/', Path.DirectorySeparatorChar)));
        if (!target.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("RomFS path outside the output folder");
        return target;
    }
    Directory.CreateDirectory(root);
    foreach (var folder in folders)
        Directory.CreateDirectory(Local(folder));
    // Largest files first, each to the least loaded worker. Every worker opens its own copy of the patched RomFS
    // (own keys, storages and NCA state: nothing is shared between threads) and keeps the same integrity check.
    var parts = Enumerable.Range(0, workers).Select(_ => new List<(string Path, long Size)>()).ToArray();
    var load = new long[workers];
    foreach (var file in files.OrderByDescending(f => f.Size))
    {
        int least = Array.IndexOf(load, load.Min());
        parts[least].Add(file);
        load[least] += file.Size;
    }
    Parallel.For(0, workers, new ParallelOptions { MaxDegreeOfParallelism = workers }, index =>
    {
        var own = KeySet.CreateDefaultKeySet();
        ExternalKeyReader.ReadKeyFile(own, args[3], null, null, null);
        ExternalKeyReader.ReadTitleKeys(own, new MemoryStream(ticketBytes, false), null);
        using var ownBase = new LocalStorage(args[0], FileAccess.Read);
        using var ownPatch = new LocalStorage(args[1], FileAccess.Read);
        using var data = new Nca(own, ownBase).OpenFileSystemWithPatch(new Nca(own, ownPatch), NcaSectionType.Data, check);
        var buffer = new byte[1 << 20];
        foreach (var (path, size) in parts[index])
        {
            string target = Local(path);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using var source = new UniqueRef<IFile>();
            data.OpenFile(ref source.Ref, path.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            using var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 16);
            long offset = 0;
            while (offset < size)
            {
                source.Get.Read(out long read, offset, buffer.AsSpan(0, (int)Math.Min(buffer.Length, size - offset))).ThrowIfFailure();
                if (read <= 0)
                    throw new EndOfStreamException("RomFS file shorter than its entry");
                output.Write(buffer, 0, (int)read);
                offset += read;
            }
        }
    });
    File.WriteAllText(Path.Combine(args[4], "romfs-size.txt"), FormattableString.Invariant($"{files.Count} {bytes}"));
    return 0;
}
catch (Exception error)
{
    // Key readers can include private values in their diagnostic output.
    if (error is AggregateException aggregate && aggregate.InnerException is not null)
        error = aggregate.InnerException;
    Console.Error.WriteLine($"{stage}: {error.GetType().FullName} (0x{error.HResult:x8})");
    return stage;
}

static class Dlc
{
    public static int Import(string input, string keyPath, string output)
    {
        try
        {
            var keys = KeySet.CreateDefaultKeySet();
            ExternalKeyReader.ReadKeyFile(keys, keyPath, null, null, null);
            string[] tickets = Directory.GetFiles(input, "*.tik");
            if (tickets.Length != 1) throw new InvalidDataException();
            using (var stream = File.OpenRead(tickets[0]))
            {
                var ticket = new Ticket(stream);
                string entry = Convert.ToHexString(ticket.RightsId).ToLowerInvariant() + " = "
                             + Convert.ToHexString(ticket.GetTitleKey(keys)).ToLowerInvariant() + "\n";
                ExternalKeyReader.ReadTitleKeys(keys, new MemoryStream(Encoding.ASCII.GetBytes(entry)), null);
            }
            const ulong title = 0x0100152000023001UL;
            const IntegrityCheckLevel check = IntegrityCheckLevel.ErrorOnInvalid;
            using var metadataStorage = new LocalStorage(Path.Combine(input, "1fa7f2036e3aabdede91fa1cd78705ca.cnmt.nca"), FileAccess.Read);
            var metadata = new Nca(keys, metadataStorage);
            if (metadata.Header.TitleId != title || metadata.Header.ContentType != NcaContentType.Meta)
                throw new InvalidDataException();
            using var metaFs = metadata.OpenFileSystem(NcaSectionType.Data, check);
            string metaOutput = Path.Combine(output, "meta");
            Extractor.Extract(metaFs, metaOutput, null);
            string[] cnmtFiles = Directory.GetFiles(metaOutput, "*.cnmt", SearchOption.AllDirectories);
            if (cnmtFiles.Length != 1) throw new InvalidDataException();
            using var cnmtStream = File.OpenRead(cnmtFiles[0]);
            var cnmt = new Cnmt(cnmtStream);
            if (cnmt.Type != LibHac.Ncm.ContentMetaType.AddOnContent || cnmt.TitleId != title ||
                cnmt.ApplicationTitleId != 0x0100152000022000UL || cnmt.TitleVersion.Version != 65536 ||
                cnmt.MinimumApplicationVersion.Version > 1507328 || cnmt.ContentEntries.Length != 1)
                throw new InvalidDataException();
            var content = cnmt.ContentEntries[0];
            string id = Convert.ToHexString(content.NcaId).ToLowerInvariant();
            if (content.Type != LibHac.Ncm.ContentType.Data || id != "a3e030a30e112d39de059a24f7c80b63" || content.Size != 114688)
                throw new InvalidDataException();
            string contentPath = Path.Combine(input, id + ".nca");
            if (new FileInfo(contentPath).Length != content.Size ||
                !SHA256.HashData(File.ReadAllBytes(contentPath)).SequenceEqual(content.Hash))
                throw new InvalidDataException();
            using var dataStorage = new LocalStorage(contentPath, FileAccess.Read);
            var data = new Nca(keys, dataStorage);
            if (data.Header.TitleId != title || data.Header.ContentType != NcaContentType.PublicData)
                throw new InvalidDataException();
            using var romfs = data.OpenFileSystem(NcaSectionType.Data, check);
            Extractor.Extract(romfs, Path.Combine(output, "romfs"), null);
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"DLC: {error.GetType().FullName} (0x{error.HResult:x8})");
            return 20;
        }
    }
}
