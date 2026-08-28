using System.IO;
using System.Windows;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs eventArgs)
    {
        base.OnStartup(eventArgs);
        ApplicationLaunchOptions options;
        try
        {
            options = ApplicationLaunchOptions.Parse(eventArgs.Args, AppContext.BaseDirectory);
        }
        catch (ArgumentException exception)
        {
            MessageBox.Show(
                exception.Message,
                "A0 Camera Stitcher — 起動引数エラー",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            Shutdown(2);
            return;
        }

        Window window;
        try
        {
            window = options.Mode switch
            {
                ApplicationLaunchMode.Simulated => new MainWindow(),
                ApplicationLaunchMode.HardwareSingle => new HardwareSingleCameraWindow(options.SingleCameraAgentExecutablePath),
                ApplicationLaunchMode.HardwareDual => new MainWindow(
                    DualCameraExecutionEnvironment.HardwareDual,
                    options.DualCameraAgentExecutablePath,
                    options.DualWpdCameraMapPath),
                _ => new LaunchWindow(options.SingleCameraAgentExecutablePath),
            };
        }
        catch (HardwareSingleAppSessionBusyException exception)
        {
            MessageBox.Show(
                exception.Message,
                "A0 Camera Stitcher — 実機操作は既に使用中",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            Shutdown(3);
            return;
        }
        MainWindow = window;
        window.Show();
    }
}

public enum ApplicationLaunchMode
{
    Launcher,
    Simulated,
    HardwareSingle,
    HardwareDual,
}

public sealed record ApplicationLaunchOptions(
    ApplicationLaunchMode Mode,
    string SingleCameraAgentExecutablePath,
    string DualCameraAgentExecutablePath,
    string? DualWpdCameraMapPath)
{
    public static ApplicationLaunchOptions Parse(IReadOnlyList<string> arguments, string baseDirectory)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        if (string.IsNullOrWhiteSpace(baseDirectory))
        {
            throw new ArgumentException("Application base directory is unavailable.", nameof(baseDirectory));
        }

        var mode = ApplicationLaunchMode.Launcher;
        string? configuredAgent = null;
        string? configuredWpdCameraMap = null;
        var modeSeen = false;
        for (var index = 0; index < arguments.Count; index++)
        {
            switch (arguments[index])
            {
                case "--simulated":
                    if (modeSeen)
                    {
                        throw new ArgumentException("起動モードは一つだけ指定してください。");
                    }

                    modeSeen = true;
                    mode = ApplicationLaunchMode.Simulated;
                    break;
                case "--hardware-single":
                    if (modeSeen)
                    {
                        throw new ArgumentException("起動モードは一つだけ指定してください。");
                    }

                    modeSeen = true;
                    mode = ApplicationLaunchMode.HardwareSingle;
                    break;
                case "--hardware-dual":
                    if (modeSeen)
                    {
                        throw new ArgumentException("起動モードは一つだけ指定してください。");
                    }
                    modeSeen = true;
                    mode = ApplicationLaunchMode.HardwareDual;
                    break;
                case "--camera-agent":
                    if (configuredAgent is not null || ++index >= arguments.Count ||
                        string.IsNullOrWhiteSpace(arguments[index]))
                    {
                        throw new ArgumentException("--camera-agent には重複しない実行ファイルパスが必要です。");
                    }

                    configuredAgent = arguments[index];
                    break;
                case "--wpd-camera-map":
                    if (configuredWpdCameraMap is not null || ++index >= arguments.Count ||
                        string.IsNullOrWhiteSpace(arguments[index]))
                    {
                        throw new ArgumentException("--wpd-camera-map には重複しない既存ファイルのパスが必要です。");
                    }

                    configuredWpdCameraMap = arguments[index];
                    break;
                default:
                    throw new ArgumentException($"未対応の起動引数です: {arguments[index]}");
            }
        }

        if (configuredAgent is not null && mode == ApplicationLaunchMode.Simulated)
        {
            throw new ArgumentException("--camera-agent はSIMULATEDモードでは指定できません。");
        }
        if (configuredWpdCameraMap is not null && mode != ApplicationLaunchMode.HardwareDual)
        {
            throw new ArgumentException("--wpd-camera-map は--hardware-dual と一緒に指定してください。");
        }
        if (mode == ApplicationLaunchMode.HardwareDual && configuredWpdCameraMap is null)
        {
            throw new ArgumentException("--hardware-dual には既存の --wpd-camera-map が必要です。");
        }

        var normalizedBase = Path.GetFullPath(baseDirectory);
        var singleDefault = Path.Combine(normalizedBase, "A0CameraStitcher.CameraAgent.exe");
        var dualDefault = Path.Combine(normalizedBase, "A0CameraStitcher.DualCameraAgent.exe");
        var singlePath = mode switch
        {
            // --hardware-single 明示指定時のみ、起動直後にEXE実在等を検証しfail-closedを維持する。
            ApplicationLaunchMode.HardwareSingle =>
                CameraAgentExecutablePolicy.Resolve(normalizedBase, configuredAgent ?? singleDefault),
            // Launcher（引数なし起動）は既定パス（singleDefault）についてのみResolveの必須対象
            // から外す。既定パスが指すEXEがパッケージから欠けていても、実在確認はLaunchWindowの
            // 表示、および実機Single画面を開く際のPersistentHardwareCameraAgentOperations.
            // AgentExecutableAvailable による独立したfail-closed判定に委ねる（issue #142 症状1）。
            // 一方 --camera-agent で明示指定された値は、Launcherモードでも配置先直下・.exe拡張子・
            // トラバーサル拒否等の封じ込めをResolveで検証する。ここを素通りさせると
            // `A0CameraStitcher.exe --camera-agent <任意のパス>` がLauncherモードのまま通り、
            // 「実機1台」クリック経由で未検証の実行ファイルを起動できてしまう。
            ApplicationLaunchMode.Launcher => configuredAgent is null
                ? singleDefault
                : CameraAgentExecutablePolicy.Resolve(normalizedBase, configuredAgent),
            _ => singleDefault,
        };
        var dualPath = mode == ApplicationLaunchMode.HardwareDual
            ? CameraAgentExecutablePolicy.Resolve(normalizedBase, configuredAgent ?? dualDefault)
            : dualDefault;
        var dualWpdMapPath = mode == ApplicationLaunchMode.HardwareDual
            ? ResolveExistingFixedLocalMap(configuredWpdCameraMap!)
            : null;
        return new ApplicationLaunchOptions(
            mode,
            singlePath,
            dualPath,
            dualWpdMapPath);
    }

    private static string ResolveExistingFixedLocalMap(string candidate)
    {
        try
        {
            var normalized = Path.GetFullPath(candidate);
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(normalized);
            if (!File.Exists(normalized))
            {
                throw new InvalidDataException("The WPD map does not exist.");
            }

            var attributes = File.GetAttributes(normalized);
            if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint | FileAttributes.Device)) != 0)
            {
                throw new InvalidDataException("The WPD map is not a regular file.");
            }

            return normalized;
        }
        catch (Exception exception) when (exception is ArgumentException or IOException or UnauthorizedAccessException or
            NotSupportedException or System.Security.SecurityException or InvalidDataException)
        {
            throw new ArgumentException("--wpd-camera-map は固定ローカルドライブ上の既存通常ファイルである必要があります。");
        }
    }
}
