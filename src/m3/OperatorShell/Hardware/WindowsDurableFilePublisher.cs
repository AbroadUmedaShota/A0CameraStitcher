using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal static partial class WindowsDurableFilePublisher
{
    private const uint DeleteAccess = 0x00010000;
    private const uint GenericRead = 0x80000000;
    private const uint GenericWrite = 0x40000000;
    private const uint FileAttributeNormal = 0x00000080;
    private const uint FileFlagSequentialScan = 0x08000000;
    private const uint FileFlagWriteThrough = 0x80000000;
    private const uint OpenExisting = 3;
    private const int FileRenameInfo = 3;
    private const uint MoveFileReplaceExisting = 0x00000001;
    private const uint MoveFileWriteThrough = 0x00000008;

    public static FileStream OpenLockedForVerifiedPublish(string sourcePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(sourcePath);
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("Verified publication requires Windows.");
        }

        var handle = CreateFile(
            sourcePath,
            GenericRead | GenericWrite | DeleteAccess,
            FileShare.Read | FileShare.Delete,
            IntPtr.Zero,
            OpenExisting,
            FileAttributeNormal | FileFlagSequentialScan | FileFlagWriteThrough,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            var error = Marshal.GetLastPInvokeError();
            handle.Dispose();
            throw new IOException(
                "The staged export could not be locked for verified publication.",
                new Win32Exception(error));
        }

        try
        {
            return new FileStream(
                handle,
                FileAccess.ReadWrite,
                bufferSize: 128 * 1024,
                isAsync: false);
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    public static void PublishLocked(
        FileStream verifiedStagingFile,
        string destinationPath,
        bool replaceExisting)
    {
        ArgumentNullException.ThrowIfNull(verifiedStagingFile);
        ArgumentException.ThrowIfNullOrWhiteSpace(destinationPath);
        if (!verifiedStagingFile.CanRead || !verifiedStagingFile.CanWrite)
        {
            throw new InvalidOperationException(
                "Verified publication requires the locked read/write staging handle.");
        }

        verifiedStagingFile.Flush(flushToDisk: true);
        RenameOpenHandle(
            verifiedStagingFile.SafeFileHandle,
            Path.GetFullPath(destinationPath),
            replaceExisting);
    }

    public static void Publish(string sourcePath, string destinationPath, bool replaceExisting)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(sourcePath);
        ArgumentException.ThrowIfNullOrWhiteSpace(destinationPath);
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("Durable file publication requires Windows.");
        }

        var flags = MoveFileWriteThrough | (replaceExisting ? MoveFileReplaceExisting : 0u);
        if (!MoveFileEx(sourcePath, destinationPath, flags))
        {
            var error = Marshal.GetLastPInvokeError();
            throw new IOException(
                "Windows did not durably publish the staged file.",
                new Win32Exception(error));
        }
    }

    private static void RenameOpenHandle(
        SafeFileHandle handle,
        string destinationPath,
        bool replaceExisting)
    {
        var fileNameBytes = Encoding.Unicode.GetBytes(destinationPath);
        var rootDirectoryOffset = IntPtr.Size == 8 ? 8 : 4;
        var fileNameLengthOffset = rootDirectoryOffset + IntPtr.Size;
        var fileNameOffset = fileNameLengthOffset + sizeof(int);
        // FILE_RENAME_INFO.FileNameLength excludes the terminator, but keeping
        // a trailing UTF-16 NUL avoids filesystem/provider implementations
        // reading past the variable-length name buffer.
        var bufferSize = checked(fileNameOffset + fileNameBytes.Length + sizeof(char));
        var buffer = Marshal.AllocHGlobal(bufferSize);
        try
        {
            for (var offset = 0; offset < bufferSize; offset++)
            {
                Marshal.WriteByte(buffer, offset, 0);
            }

            Marshal.WriteByte(buffer, 0, replaceExisting ? (byte)1 : (byte)0);
            Marshal.WriteIntPtr(buffer, rootDirectoryOffset, IntPtr.Zero);
            Marshal.WriteInt32(buffer, fileNameLengthOffset, fileNameBytes.Length);
            Marshal.Copy(fileNameBytes, 0, IntPtr.Add(buffer, fileNameOffset), fileNameBytes.Length);
            if (!SetFileInformationByHandle(
                    handle,
                    FileRenameInfo,
                    buffer,
                    checked((uint)bufferSize)))
            {
                var error = Marshal.GetLastPInvokeError();
                throw new IOException(
                    "Windows did not publish the exact verified staging handle.",
                    new Win32Exception(error));
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    [LibraryImport("kernel32.dll", EntryPoint = "CreateFileW", SetLastError = true,
        StringMarshalling = StringMarshalling.Utf16)]
    private static partial SafeFileHandle CreateFile(
        string fileName,
        uint desiredAccess,
        FileShare shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [LibraryImport("kernel32.dll", EntryPoint = "SetFileInformationByHandle", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetFileInformationByHandle(
        SafeFileHandle file,
        int fileInformationClass,
        IntPtr fileInformation,
        uint bufferSize);

    [LibraryImport("kernel32.dll", EntryPoint = "MoveFileExW", SetLastError = true,
        StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool MoveFileEx(
        string existingFileName,
        string newFileName,
        uint flags);
}
