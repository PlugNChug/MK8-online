using System;
using System.IO;
using LibHac.Common.Keys;
using LibHac.FsSystem;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using Extractor = LibHac.Tools.FsSystem.FileSystemExtensions;

if (args.Length != 5)
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
    tickets.Position = 0;
    ExternalKeyReader.ReadTitleKeys(keys, tickets, null);

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
    long count = 0, bytes = 0;
    foreach (var entry in Extractor.EnumerateEntries(romfs, "/", "*", SearchOptions.RecurseSubdirectories))
    {
        if (entry.Type != LibHac.Fs.DirectoryEntryType.File)
            continue;
        count++;
        bytes = checked(bytes + entry.Size);
    }
    if (count == 0)
        return 7;
    if (new DriveInfo(Path.GetPathRoot(args[4])!).AvailableFreeSpace < bytes + (4L << 30))
        return 8;
    stage = 10;
    Extractor.Extract(romfs, Path.Combine(args[4], "romfs"), null);
    File.WriteAllText(Path.Combine(args[4], "romfs-size.txt"), FormattableString.Invariant($"{count} {bytes}"));
    return 0;
}
catch (Exception error)
{
    // Key readers can include private values in their diagnostic output.
    Console.Error.WriteLine($"{stage}: {error.GetType().FullName} (0x{error.HResult:x8})");
    return stage;
}
