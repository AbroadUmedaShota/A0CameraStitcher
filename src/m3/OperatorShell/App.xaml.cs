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
                    options.DualCameraAgentExecutablePath),
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
    string DualCameraAgentExecutablePath)
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
                default:
                    throw new ArgumentException($"未対応の起動引数です: {arguments[index]}");
            }
        }

        if (configuredAgent is not null && mode == ApplicationLaunchMode.Simulated)
        {
            throw new ArgumentException("--camera-agent はSIMULATEDモードでは指定できません。");
        }

        var normalizedBase = Path.GetFullPath(baseDirectory);
        var singleDefault = Path.Combine(normalizedBase, "A0CameraStitcher.CameraAgent.exe");
        var dualDefault = Path.Combine(normalizedBase, "A0CameraStitcher.DualCameraAgent.exe");
        var singlePath = mode is ApplicationLaunchMode.HardwareSingle or ApplicationLaunchMode.Launcher
            ? CameraAgentExecutablePolicy.Resolve(normalizedBase, configuredAgent ?? singleDefault)
            : singleDefault;
        var dualPath = mode == ApplicationLaunchMode.HardwareDual
            ? CameraAgentExecutablePolicy.Resolve(normalizedBase, configuredAgent ?? dualDefault)
            : dualDefault;
        return new ApplicationLaunchOptions(
            mode,
            singlePath,
            dualPath);
    }
}
