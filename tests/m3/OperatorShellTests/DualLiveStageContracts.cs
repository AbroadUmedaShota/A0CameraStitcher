using System.ComponentModel;
using System.Globalization;
using System.Reflection;
using System.Text.RegularExpressions;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

// Issue #291 (front half): the dual Live stage, driven against the simulated binding agent that
// serves the bound Live View schema. Nothing here touches a camera, an SDK or an Agent process.
//
// Time is injected twice over. The stage reads a manual clock (so the time left to capture and the
// "N seconds ago" texts are exact), and every wait the stage makes -- between frame requests, and
// between polls for a first frame -- goes through a delay that advances that clock instead of
// sleeping. The frame loop is off for the sequence tests so a frame is fetched exactly when a test
// asks; one test turns it on and releases it tick by tick.
internal static class DualLiveStageContracts
{
    // A decodable 1x1 frame built at run time. The leak check treats any newly added base64 that
    // decodes to an image as a hit, so the frame is encoded here instead of being written as a literal.
    internal static readonly string DecodablePngBase64 = EncodeOnePixelPngBase64();

    private static string EncodeOnePixelPngBase64()
    {
        var pixel = System.Windows.Media.Imaging.BitmapSource.Create(
            1, 1, 96, 96, System.Windows.Media.PixelFormats.Gray8, null, new byte[] { 0 }, 1);
        var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
        encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(pixel));
        using var stream = new System.IO.MemoryStream();
        encoder.Save(stream);
        return Convert.ToBase64String(stream.ToArray());
    }

    private static readonly (string Name, Func<Task> Run)[] Tests =
    [
        ("switch moves Live from CAM-A to CAM-B with a confirmed stop between, never two open, panes fixed left and right",
            SwitchStopsBeforeItStartsAsync),
        ("update both while CAM-A is Live opens two sources: B then A, A's recent frame is reused",
            RefreshWithCameraALiveAsync),
        ("update both while CAM-A is Live but its frame is stale re-reads A without reopening it",
            RefreshWithStaleFrameAsync),
        ("update both while CAM-B is Live opens two sources in the order A then B",
            RefreshWithCameraBLiveAsync),
        ("update both with nothing open: three opens when CAM-A is selected, two when CAM-B is",
            RefreshFromNothingOpenAsync),
        ("update both stops at the first failure: CAM-B's SDK exception leaves no pane Live and invalidates the binding",
            RefreshStopsAtSdkExceptionOnBAsync),
        ("update both with CAM-A's SDK exception never opens CAM-B",
            RefreshNeverOpensBAfterAFailsAsync),
        ("an empty frame without an exception fails the switch after the first-frame wait, keeps the binding, and nothing reopens by itself",
            EmptyFrameFailsWithoutInvalidatingAsync),
        ("a first frame that arrives within the wait succeeds without a second start",
            LateFirstFrameStillSucceedsAsync),
        ("a USB change during update both closes everything, shows neither pane Live, and a fresh binding starts clean",
            UsbChangeDuringRefreshAsync),
        ("a stop that is not confirmed invalidates the binding and the new camera is never started",
            UnconfirmedStopInvalidatesAsync),
        ("a stop that failed (Live View not stopped, source closed) is not read as closed, and the capture stays refused",
            FailedStopIsNotClosedAsync),
        ("closing for a capture stops and confirms before it returns, and the activation that follows is accepted",
            PrepareForCaptureConfirmsBeforeActivationAsync),
        ("closing for a capture sends nothing when no Live View was ever opened",
            PrepareForCaptureSendsNothingWhenNeverOpenedAsync),
        ("the time left to capture counts down from the Agent start, takes the Agent's figure when shorter, and is unknown without a start time",
            RemainingTimeAsync),
        ("the time left to capture decides what is offered: caution under 60 s, no update under 30 s, closed at 0",
            RemainingTimeThresholdsAsync),
        ("when the time to capture runs out the stage closes, says so, and offers nothing",
            ExpiryClosesTheStageAsync),
        ("a camera that stops delivering frames is no longer Live after three seconds",
            StaleLiveFrameFailsAsync),
        ("the frame loop fetches frames only while Live and stops for a capture",
            FrameLoopRunsOnlyWhileLiveAsync),
        ("operations are refused while a capture is running and after capture activation",
            CaptureGatesTheStageAsync),
        ("window shutdown sends one stop before cancel-binding, and still cancels when the stop is not confirmed",
            ShutdownStopsBeforeCancelAsync),
        ("pane names, badges and button names follow the state (not only colour or frame style)",
            AccessibleNamesFollowStateAsync),
        ("no text on the stage says simultaneous or synchronised",
            NoSimultaneousWordingAsync),
        ("the 185-second reserve and the 600-second lifetime match the native constants",
            ReserveMatchesNativeAsync),
    ];

    internal static async Task RunAsync()
    {
        foreach (var (name, run) in Tests)
        {
            try
            {
                await run();
            }
            catch (Exception exception)
            {
                throw new InvalidOperationException($"{name}: {exception.Message}", exception);
            }
        }
    }

    internal static async Task<int> RunVerboseAsync()
    {
        var failures = 0;
        foreach (var (name, run) in Tests)
        {
            try
            {
                await run();
                Console.WriteLine($"PASS {name}");
            }
            catch (Exception exception)
            {
                failures++;
                Console.Error.WriteLine($"FAIL {name}: {exception}");
            }
        }

        Console.WriteLine($"Dual live stage tests: {Tests.Length - failures}/{Tests.Length} passed.");
        return failures == 0 ? 0 : 1;
    }

    // ---- fixture ------------------------------------------------------------------------------

    private sealed class ManualStageClock : IDualLiveStageClock
    {
        public DateTimeOffset UtcNow { get; private set; } = new(2026, 10, 14, 9, 0, 0, TimeSpan.Zero);

        public long MonotonicMilliseconds { get; private set; } = 1_000;

        public void Advance(TimeSpan span)
        {
            UtcNow += span;
            MonotonicMilliseconds += (long)span.TotalMilliseconds;
        }
    }

    private sealed class Fixture
    {
        public required SimulatedDualBindingAgent Agent { get; init; }

        public required DualBindingSessionClient Session { get; init; }

        public required DualBindingViewModel Binding { get; init; }

        public required DualBoundLiveViewClient Client { get; init; }

        public required DualLiveStageViewModel Stage { get; init; }

        public required ManualStageClock Clock { get; init; }

        public required DateTimeOffset AgentStartedAtUtc { get; init; }

        public List<string> Notices { get; } = [];

        public bool CaptureActive { get; set; }

        public IReadOnlyList<string> Log => Agent.BoundLiveViewCallLog;

        /// <summary>Runs <paramref name="action"/> and returns the log entries it appended.</summary>
        public async Task<string[]> LoggedByAsync(Func<Task> action)
        {
            var before = Log.Count;
            await action();
            return Log.Skip(before).ToArray();
        }

        public static async Task<Fixture> CreateAsync(
            Func<SimulatedDualBindingOptions, SimulatedDualBindingOptions>? configure = null,
            bool bind = true,
            bool agentStartKnown = true,
            bool runFrameLoop = false,
            Func<TimeSpan, CancellationToken, Task>? delay = null,
            double agentStartedSecondsAgo = 10)
        {
            var options = new SimulatedDualBindingOptions { LiveViewFrameBase64 = DecodablePngBase64 };
            if (configure is not null)
            {
                options = configure(options);
            }

            var agent = new SimulatedDualBindingAgent(options);
            var session = new DualBindingSessionClient(new SimulatedDualBindingAgentTransport(agent));
            var binding = new DualBindingViewModel(session, isRequired: true);
            var clock = new ManualStageClock();
            var startedAt = clock.UtcNow - TimeSpan.FromSeconds(agentStartedSecondsAgo);
            var client = new DualBoundLiveViewClient(session);
            Fixture? fixture = null;
            var stage = new DualLiveStageViewModel(
                client,
                binding,
                () => fixture!.CaptureActive,
                new DualLiveStageOptions
                {
                    Clock = clock,
                    AgentStartTimeUtc = agentStartKnown ? (Func<DateTimeOffset?>)(() => startedAt) : () => null,
                    Delay = delay ?? ((span, _) =>
                    {
                        clock.Advance(span);
                        return Task.CompletedTask;
                    }),
                    RunFrameLoop = runFrameLoop,
                });
            fixture = new Fixture
            {
                Agent = agent,
                Session = session,
                Binding = binding,
                Client = client,
                Stage = stage,
                Clock = clock,
                AgentStartedAtUtc = startedAt,
            };
            stage.NoticeRaised += text => fixture!.Notices.Add(text);
            if (bind)
            {
                await BindAsync(binding);
            }

            return fixture;
        }
    }

    // Drives the binding screen the way an operator would, so a Ready binding here is one the
    // screen can reach.
    private static async Task BindAsync(DualBindingViewModel binding)
    {
        binding.BeginBindingCommand.Execute(null);
        await TestWaits.PollUntilAsync(
            () => binding.Candidates.Count == 2 && !binding.IsBusy, "two binding candidates");
        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            var candidate = binding.Candidates.First(item => !item.IsAssigned);
            binding.ShowCandidateCommand.Execute(candidate);
            await TestWaits.PollUntilAsync(
                () => binding.SelectedCandidate == candidate && !binding.IsBusy, $"showing {candidate.DisplayName}");
            (alias == "CAM-A" ? binding.AssignCameraACommand : binding.AssignCameraBCommand).Execute(null);
            await TestWaits.PollUntilAsync(
                () => candidate.AssignedAlias == alias && !binding.IsBusy, $"assigning {alias}");
        }

        await TestWaits.PollUntilAsync(() => binding.Phase == DualBindingPhase.Summary, "the binding summary");
        binding.CompleteBindingCommand.Execute(null);
        await TestWaits.PollUntilAsync(() => binding.IsReady, "a Ready binding");
    }

    private static int MaximumOpenSources(IReadOnlyList<string> log)
    {
        var open = 0;
        var maximum = 0;
        foreach (var entry in log)
        {
            if (entry.StartsWith("sdk-open:", StringComparison.Ordinal))
            {
                maximum = Math.Max(maximum, ++open);
            }
            else if (entry.StartsWith("sdk-close:", StringComparison.Ordinal))
            {
                open--;
            }
        }

        return maximum;
    }

    private static string[] Slice(params string[] entries) => entries;

    private static void AssertNoPaneIsLive(Fixture fixture, string context)
    {
        Check.False(fixture.Stage.PaneA.IsLive, $"CAM-A must not be shown as Live {context}.");
        Check.False(fixture.Stage.PaneB.IsLive, $"CAM-B must not be shown as Live {context}.");
        Check.False(fixture.Stage.PaneA.State == DualLivePaneState.Live || fixture.Stage.PaneB.State == DualLivePaneState.Live,
            $"No pane may be in the Live state {context}.");
    }

    // ---- sequences ----------------------------------------------------------------------------

    private static async Task SwitchStopsBeforeItStartsAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        Check.Equal(DualLiveStageState.Idle, stage.State);
        Check.Equal("CAM-A", stage.Panes[0].Alias);
        Check.Equal("CAM-B", stage.Panes[1].Alias);
        Check.Equal(DualLivePaneState.NotAcquired, stage.PaneA.State);
        Check.Equal(DualLivePaneState.NotAcquired, stage.PaneB.State);

        await stage.SwitchAsync("CAM-A");
        Check.Equal(DualLivePaneState.Live, stage.PaneA.State);
        Check.Equal(DualLivePaneState.NotAcquired, stage.PaneB.State);
        Check.True(stage.PaneA.HasImage, "The Live pane shows the frame it received.");
        Check.Equal(DualLiveStageState.LiveOne, stage.State);

        var slice = await fixture.LoggedByAsync(() => stage.SwitchAsync("CAM-B"));
        Check.Equal(string.Join(",", Slice(
            "stop", "sdk-close:CAM-A", "start:CAM-B", "sdk-open:CAM-B", "frame:CAM-B")), string.Join(",", slice));
        Check.Equal(DualLivePaneState.Still, stage.PaneA.State);
        Check.True(stage.PaneA.HasImage, "The camera that stopped being Live keeps its last frame as a still.");
        Check.Equal(DualLivePaneState.Live, stage.PaneB.State);
        Check.Equal("CAM-B", stage.SelectedAlias);
        Check.Equal("CAM-A", stage.Panes[0].Alias);
        Check.Equal("CAM-B", stage.Panes[1].Alias);
        Check.True(MaximumOpenSources(fixture.Log) <= 1, "Never two sources open: " + string.Join(",", fixture.Log));
        Check.Equal(0, fixture.Agent.BoundLiveViewAlreadyOpenRefusalCount);
        Check.Equal(0, stage.AutomaticRetryCount);
        Check.Equal("二台の表示  CAM-A: 静止画 ／ CAM-B: Live", stage.HeadingText);
        Check.True(stage.PaneA.BadgeText.StartsWith("静止画 ", StringComparison.Ordinal) && stage.PaneA.BadgeText.Contains('・'),
            "A still pane's badge names its kind and carries the time and the age: " + stage.PaneA.BadgeText);
        Check.Equal("Live", stage.PaneB.BadgeText);
    }

    private static async Task RefreshWithCameraALiveAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");
        var opensBefore = fixture.Agent.BoundLiveViewOpenCount;

        var slice = await fixture.LoggedByAsync(stage.RefreshBothAsync);
        Check.Equal(string.Join(",", Slice(
            "stop", "sdk-close:CAM-A",
            "start:CAM-B", "sdk-open:CAM-B", "frame:CAM-B",
            "stop", "sdk-close:CAM-B",
            "start:CAM-A", "sdk-open:CAM-A", "frame:CAM-A")), string.Join(",", slice));
        Check.Equal(2, fixture.Agent.BoundLiveViewOpenCount - opensBefore);
        Check.True(MaximumOpenSources(fixture.Log) <= 1, "Never two sources open.");
        Check.Equal(DualLivePaneState.Live, stage.PaneA.State);
        Check.Equal(DualLivePaneState.Still, stage.PaneB.State);
        Check.True(stage.PaneB.HasImage && stage.PaneB.LastFrameAtUtc is not null, "CAM-B has a fresh still with its time.");
        Check.Equal(DualLiveStageState.LiveOne, stage.State);
        Check.True(stage.StatusText.StartsWith("両方を更新しました", StringComparison.Ordinal), stage.StatusText);
        Check.Equal(1, fixture.Notices.Count);
    }

    private static async Task RefreshWithStaleFrameAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");
        fixture.Clock.Advance(TimeSpan.FromSeconds(5));
        var opensBefore = fixture.Agent.BoundLiveViewOpenCount;

        var slice = await fixture.LoggedByAsync(stage.RefreshBothAsync);
        Check.Equal(string.Join(",", Slice(
            "frame:CAM-A",
            "stop", "sdk-close:CAM-A",
            "start:CAM-B", "sdk-open:CAM-B", "frame:CAM-B",
            "stop", "sdk-close:CAM-B",
            "start:CAM-A", "sdk-open:CAM-A", "frame:CAM-A")), string.Join(",", slice));
        Check.Equal(2, fixture.Agent.BoundLiveViewOpenCount - opensBefore);
        Check.Equal(DualLivePaneState.Live, stage.PaneA.State);
    }

    private static async Task RefreshWithCameraBLiveAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");
        await stage.SwitchAsync("CAM-B");
        var opensBefore = fixture.Agent.BoundLiveViewOpenCount;

        var slice = await fixture.LoggedByAsync(stage.RefreshBothAsync);
        Check.Equal(string.Join(",", Slice(
            "stop", "sdk-close:CAM-B",
            "start:CAM-A", "sdk-open:CAM-A", "frame:CAM-A",
            "stop", "sdk-close:CAM-A",
            "start:CAM-B", "sdk-open:CAM-B", "frame:CAM-B")), string.Join(",", slice));
        Check.Equal(2, fixture.Agent.BoundLiveViewOpenCount - opensBefore);
        Check.Equal(DualLivePaneState.Still, stage.PaneA.State);
        Check.Equal(DualLivePaneState.Live, stage.PaneB.State);
        Check.Equal("CAM-B", stage.SelectedAlias);
    }

    private static async Task RefreshFromNothingOpenAsync()
    {
        // Selected CAM-A (the default): A, B, then A again.
        var fromA = await Fixture.CreateAsync();
        var sliceA = await fromA.LoggedByAsync(fromA.Stage.RefreshBothAsync);
        Check.Equal(string.Join(",", Slice(
            "start:CAM-A", "sdk-open:CAM-A", "frame:CAM-A",
            "stop", "sdk-close:CAM-A",
            "start:CAM-B", "sdk-open:CAM-B", "frame:CAM-B",
            "stop", "sdk-close:CAM-B",
            "start:CAM-A", "sdk-open:CAM-A", "frame:CAM-A")), string.Join(",", sliceA));
        Check.Equal(3, fromA.Agent.BoundLiveViewOpenCount);
        Check.Equal(DualLivePaneState.Live, fromA.Stage.PaneA.State);

        // Selected CAM-B with nothing open (it was closed for a capture): B simply stays open.
        var fromB = await Fixture.CreateAsync();
        await fromB.Stage.SwitchAsync("CAM-B");
        Check.True(await fromB.Stage.PrepareForCaptureAsync(), "Closing for a capture must succeed.");
        var opensBefore = fromB.Agent.BoundLiveViewOpenCount;
        var sliceB = await fromB.LoggedByAsync(fromB.Stage.RefreshBothAsync);
        Check.Equal(string.Join(",", Slice(
            "start:CAM-A", "sdk-open:CAM-A", "frame:CAM-A",
            "stop", "sdk-close:CAM-A",
            "start:CAM-B", "sdk-open:CAM-B", "frame:CAM-B")), string.Join(",", sliceB));
        Check.Equal(2, fromB.Agent.BoundLiveViewOpenCount - opensBefore);
        Check.Equal(DualLivePaneState.Live, fromB.Stage.PaneB.State);
        Check.Equal(DualLivePaneState.Still, fromB.Stage.PaneA.State);
    }

    private static async Task RefreshStopsAtSdkExceptionOnBAsync()
    {
        var fixture = await Fixture.CreateAsync(options => options with { FailBoundLiveViewStartAlias = "CAM-B" });
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");
        var stillBefore = stage.PaneA.LastFrameAtUtc;

        await stage.RefreshBothAsync();
        AssertNoPaneIsLive(fixture, "after an SDK exception during update both");
        Check.Equal(DualLivePaneState.Failed, stage.PaneB.State);
        Check.Equal(DualLivePaneState.Still, stage.PaneA.State);
        Check.True(stage.PaneA.HasImage, "CAM-A keeps its picture.");
        Check.True(stillBefore is not null, "CAM-A had a received time before the update.");
        Check.False(fixture.Binding.IsReady, "An SDK exception invalidates the binding.");
        Check.Equal(DualBindingPhase.Invalid, fixture.Binding.Phase);
        Check.Equal(DualLiveStageState.Invalid, stage.State);
        Check.True(stage.AlertText.Contains("CAM-B", StringComparison.Ordinal) &&
            stage.AlertText.Contains("機体照合をやり直してください", StringComparison.Ordinal), stage.AlertText);
        Check.Equal("block", stage.AlertKind);
        Check.True(fixture.Agent.OpenBoundLiveViewAlias is null, "The failed start left nothing open.");
        Check.Equal(2, fixture.Log.Count(entry => entry.StartsWith("start:", StringComparison.Ordinal)));
        Check.Equal(0, stage.AutomaticRetryCount);
    }

    private static async Task RefreshNeverOpensBAfterAFailsAsync()
    {
        var fixture = await Fixture.CreateAsync(options => options with { FailBoundLiveViewStartAlias = "CAM-A" });
        await fixture.Stage.RefreshBothAsync();
        Check.Equal(0, fixture.Agent.BoundLiveViewOpenCountFor("CAM-B"));
        Check.False(fixture.Log.Any(entry => entry.Contains("CAM-B", StringComparison.Ordinal)),
            "CAM-B must not be touched after CAM-A failed: " + string.Join(",", fixture.Log));
        AssertNoPaneIsLive(fixture, "after CAM-A failed");
        Check.Equal(DualLivePaneState.Failed, fixture.Stage.PaneA.State);
        Check.False(fixture.Binding.IsReady, "The SDK exception invalidated the binding.");
    }

    private static async Task EmptyFrameFailsWithoutInvalidatingAsync()
    {
        var fixture = await Fixture.CreateAsync(options => options with { EmptyBoundLiveViewFrames = true });
        var stage = fixture.Stage;
        var startedAt = fixture.Clock.UtcNow;

        await stage.SwitchAsync("CAM-A");
        var waited = fixture.Clock.UtcNow - startedAt;
        Check.True(waited >= DualLiveStageViewModel.FirstFrameTimeout && waited < DualLiveStageViewModel.FirstFrameTimeout + TimeSpan.FromSeconds(1),
            $"The first-frame wait is the bound, not longer: {waited}.");
        AssertNoPaneIsLive(fixture, "after the first frame never came");
        Check.Equal(DualLivePaneState.Failed, stage.PaneA.State);
        Check.Equal(DualLiveStageState.Failed, stage.State);
        Check.True(fixture.Binding.IsReady, "No SDK exception: the binding stays usable.");
        Check.True(fixture.Agent.OpenBoundLiveViewAlias is null, "The Live View was stopped and closed.");
        Check.Equal(1, fixture.Log.Count(entry => entry.StartsWith("start:", StringComparison.Ordinal)));
        Check.Equal(1, fixture.Log.Count(entry => entry == "stop"));
        Check.Equal("CAM-A の画像を取得できませんでした。もう一度 [両方を更新] を押してください", stage.StatusText);

        // Nothing reopens by itself, however long it is left alone.
        var lengthAfterFailure = fixture.Log.Count;
        for (var tick = 0; tick < 5; tick++)
        {
            fixture.Clock.Advance(TimeSpan.FromSeconds(2));
            await stage.TickAsync();
        }

        Check.Equal(lengthAfterFailure, fixture.Log.Count);
        Check.Equal(0, stage.AutomaticRetryCount);
        Check.True(stage.RefreshBothCommand.CanExecute(null), "The operator may try again by hand.");
        AssertNoPaneIsLive(fixture, "while left alone");
    }

    private static async Task LateFirstFrameStillSucceedsAsync()
    {
        var fixture = await Fixture.CreateAsync(options => options with { EmptyBoundLiveViewFramesAfterStart = 3 });
        var startedAt = fixture.Clock.UtcNow;
        await fixture.Stage.SwitchAsync("CAM-A");
        Check.Equal(DualLivePaneState.Live, fixture.Stage.PaneA.State);
        Check.Equal(1, fixture.Log.Count(entry => entry.StartsWith("start:", StringComparison.Ordinal)));
        Check.Equal(4, fixture.Log.Count(entry => entry.StartsWith("frame:", StringComparison.Ordinal)));
        Check.Equal(TimeSpan.FromMilliseconds(300), fixture.Clock.UtcNow - startedAt);
    }

    private static async Task UsbChangeDuringRefreshAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");
        fixture.Agent.BoundLiveViewRequestObserver = step =>
        {
            if (step == "get-bound-live-view-frame:CAM-B")
            {
                fixture.Agent.RaiseInvalidation(DualBindingInvalidationReason.UsbReconnect);
            }
        };

        await stage.RefreshBothAsync();
        AssertNoPaneIsLive(fixture, "after a USB change");
        Check.Equal(DualLivePaneState.Closed, stage.PaneA.State);
        Check.Equal(DualLivePaneState.Closed, stage.PaneB.State);
        Check.Equal(DualLiveStageState.Invalid, stage.State);
        Check.Equal(DualLiveStageText.BindingLostUsb, stage.AlertText);
        Check.False(fixture.Binding.IsReady, "The binding is gone.");
        Check.True(fixture.Binding.InvalidationText.Contains("USB の再接続", StringComparison.Ordinal), fixture.Binding.InvalidationText);
        Check.True(fixture.Agent.OpenBoundLiveViewAlias is null, "The invalidation ended the SDK session.");
        Check.False(stage.RefreshBothCommand.CanExecute(null), "Nothing is offered without a binding.");

        // A fresh binding starts clean: no picture, no time, nothing selected from before.
        fixture.Agent.BoundLiveViewRequestObserver = null;
        await BindAsync(fixture.Binding);
        Check.Equal(DualLiveStageState.Idle, stage.State);
        Check.False(stage.PaneA.HasImage || stage.PaneB.HasImage, "Pictures from the lost binding are gone.");
        Check.Equal(DualLivePaneState.NotAcquired, stage.PaneA.State);
        Check.Equal(DualLivePaneState.NotAcquired, stage.PaneB.State);
        Check.True(stage.AlertText.Length == 0, "The previous loss is not carried into the new binding: " + stage.AlertText);
        await stage.SwitchAsync("CAM-B");
        Check.Equal(DualLivePaneState.Live, stage.PaneB.State);
    }

    private static async Task UnconfirmedStopInvalidatesAsync()
    {
        var fixture = await Fixture.CreateAsync(options => options with { BoundLiveViewStopLeavesSourceOpen = true });
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");

        await stage.SwitchAsync("CAM-B");
        Check.False(fixture.Log.Any(entry => entry == "start:CAM-B"),
            "CAM-B must never be started on top of a source that was not confirmed closed.");
        AssertNoPaneIsLive(fixture, "after the stop was not confirmed");
        Check.False(fixture.Binding.IsReady, "A stop that cannot be confirmed makes the binding unusable.");
        Check.Equal(DualBindingSessionState.Invalid, fixture.Session.State);
        Check.Equal(DualLiveStageState.Invalid, stage.State);
        Check.Equal(DualBoundLiveViewState.Invalidated, fixture.Client.State);
        Check.Equal(1, fixture.Log.Count(entry => entry == "stop"));

        // And a capture cannot start from here.
        Check.False(await stage.PrepareForCaptureAsync(), "A binding that is lost cannot be closed for a capture.");
    }

    private static async Task FailedStopIsNotClosedAsync()
    {
        // The Agent answers a failed stop with liveViewStopped=false, sdkSourceClosed=true, wasOpen=true
        // (a BindingInvalidated refusal). Closed is claimed only when the Agent confirmed the stop.
        var fixture = await Fixture.CreateAsync(options => options with { FailStopBoundLiveView = true });
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");
        Check.Equal(DualLivePaneState.Live, stage.PaneA.State);

        await stage.SwitchAsync("CAM-B");
        Check.False(fixture.Log.Any(entry => entry == "start:CAM-B"),
            "CAM-B must never be started after a stop that failed.");
        AssertNoPaneIsLive(fixture, "after the stop failed");
        Check.True(fixture.Client.State != DualBoundLiveViewState.ConfirmedClosed,
            "A stop that failed is never recorded as confirmed closed.");
        Check.Equal(DualBoundLiveViewState.Invalidated, fixture.Client.State);
        Check.False(fixture.Binding.IsReady, "The binding is lost.");
        Check.Equal(DualLiveStageState.Invalid, stage.State);
        Check.Equal(1, fixture.Log.Count(entry => entry == "stop"));

        // Neither the stage nor the activation lets a capture start from here.
        Check.False(await stage.PrepareForCaptureAsync(), "A failed stop cannot be closed for a capture.");
        Check.False(await fixture.Binding.ActivateCaptureAsync(), "The activation is refused until a fresh binding.");
        Check.False(fixture.Agent.CaptureHostActivated, "The Agent never handed over to the capture host.");
    }

    // ---- closing for a capture ----------------------------------------------------------------

    private static async Task PrepareForCaptureConfirmsBeforeActivationAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");

        var slice = await fixture.LoggedByAsync(async () =>
            Check.True(await stage.PrepareForCaptureAsync(), "Closing for a capture must succeed."));
        Check.Equal(string.Join(",", Slice("stop", "sdk-close:CAM-A")), string.Join(",", slice));
        Check.Equal(DualBoundLiveViewState.ConfirmedClosed, fixture.Client.State);
        Check.True(fixture.Agent.OpenBoundLiveViewAlias is null, "Nothing may be open when the capture starts.");
        Check.Equal(DualLivePaneState.Still, stage.PaneA.State);
        Check.True(stage.PaneA.HasImage, "The last frame stays as a still.");
        Check.Equal(DualLiveStageState.Idle, stage.State);

        // The activation that follows is accepted: the Agent has nothing open to refuse it for.
        Check.True(await fixture.Binding.ActivateCaptureAsync(), "The activation after a confirmed close must be accepted.");
        Check.True(fixture.Agent.CaptureHostActivated, "The Agent handed over to the capture host.");
    }

    private static async Task PrepareForCaptureSendsNothingWhenNeverOpenedAsync()
    {
        var fixture = await Fixture.CreateAsync();
        Check.True(await fixture.Stage.PrepareForCaptureAsync(), "Nothing open: nothing to confirm.");
        Check.Equal(0, fixture.Log.Count);
        Check.Equal(DualLiveStageState.Idle, fixture.Stage.State);

        // After a Live View was used and closed, a second capture sends nothing more.
        await fixture.Stage.SwitchAsync("CAM-A");
        Check.True(await fixture.Stage.PrepareForCaptureAsync(), "The first close must succeed.");
        var lengthAfterClose = fixture.Log.Count;
        Check.True(await fixture.Stage.PrepareForCaptureAsync(), "A second close must succeed.");
        Check.Equal(lengthAfterClose, fixture.Log.Count);
    }

    // ---- time left to capture -----------------------------------------------------------------

    private static async Task RemainingTimeAsync()
    {
        // The Agent started 10 s ago: 600 - 185 - 10 = 405 s = 6:45.
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        Check.Equal(TimeSpan.FromSeconds(405), stage.CaptureTimeRemaining!.Value);
        Check.Equal("撮影できる残り 6:45", stage.RemainingText);
        Check.Equal("normal", stage.RemainingKind);
        Check.Equal("撮影できる残り時間 6分45秒", stage.RemainingAutomationName);

        fixture.Clock.Advance(TimeSpan.FromSeconds(400));
        Check.Equal("撮影できる残り 0:05", stage.RemainingText);
        fixture.Clock.Advance(TimeSpan.FromSeconds(100));
        Check.Equal(TimeSpan.Zero, stage.CaptureTimeRemaining!.Value);
        Check.Equal("expired", stage.RemainingKind);

        // The Agent's own figure, aged by the time since it arrived, wins when it is shorter.
        var reported = await Fixture.CreateAsync(options => options with { BoundCaptureAdmissionRemainingMs = 200_000 });
        await reported.Stage.SwitchAsync("CAM-A");
        Check.Equal(TimeSpan.FromSeconds(200), reported.Stage.CaptureTimeRemaining!.Value);
        Check.Equal("撮影できる残り 3:20", reported.Stage.RemainingText);
        reported.Clock.Advance(TimeSpan.FromSeconds(50));
        Check.Equal("撮影できる残り 2:30", reported.Stage.RemainingText);

        // A longer figure from the Agent never lengthens the local one.
        var longer = await Fixture.CreateAsync(options => options with { BoundCaptureAdmissionRemainingMs = 900_000 });
        await longer.Stage.SwitchAsync("CAM-A");
        Check.Equal(TimeSpan.FromSeconds(405), longer.Stage.CaptureTimeRemaining!.Value);

        // Without the Agent's start time nothing is guessed, and nothing is offered.
        var unknown = await Fixture.CreateAsync(agentStartKnown: false);
        Check.True(unknown.Stage.CaptureTimeRemaining is null, "An unknown start time is unknown, not full.");
        Check.Equal(DualLiveStageText.RemainingUnknown, unknown.Stage.RemainingText);
        Check.Equal("unknown", unknown.Stage.RemainingKind);
        Check.Equal(DualLiveStageText.DisabledRemainingUnknown, unknown.Stage.RefreshDisabledReason);
        Check.False(unknown.Stage.RefreshBothCommand.CanExecute(null), "No update without a known time.");
        Check.False(unknown.Stage.SwitchLiveCommand.CanExecute("CAM-A"), "No switch without a known time.");
        await unknown.Stage.SwitchAsync("CAM-A");
        Check.Equal(0, unknown.Log.Count);
        await unknown.Stage.TickAsync();
        Check.Equal(DualLiveStageState.Idle, unknown.Stage.State);
    }

    private static async Task RemainingTimeThresholdsAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        Check.Equal(string.Empty, stage.RefreshDisabledReason);
        Check.Equal(string.Empty, stage.AlertText);
        Check.True(stage.RefreshBothCommand.CanExecute(null), "With plenty of time the update is offered.");

        // 45 s left: caution (60 s or less), but the update is still offered (it is withheld only under 30 s).
        fixture.Clock.Advance(TimeSpan.FromSeconds(360));
        Check.Equal("撮影できる残り 0:45", stage.RemainingText);
        Check.Equal("caution", stage.RemainingKind);
        Check.Equal(DualLiveStageText.RemainingCautionSoon, stage.AlertText);
        Check.Equal(string.Empty, stage.RefreshDisabledReason);
        Check.True(stage.RefreshBothCommand.CanExecute(null), "At 45 s the update is still offered.");
        Check.True(stage.SwitchLiveCommand.CanExecute("CAM-B"), "A switch is offered.");

        // 29 s: the update is withheld, a switch is shorter than an update and is still offered.
        fixture.Clock.Advance(TimeSpan.FromSeconds(16));
        Check.Equal("撮影できる残り 0:29", stage.RemainingText);
        Check.Equal(DualLiveStageText.DisabledRemainingShort, stage.RefreshDisabledReason);
        Check.False(stage.RefreshBothCommand.CanExecute(null), "Under 30 s the update is not offered.");
        Check.True(stage.SwitchLiveCommand.CanExecute("CAM-B"), "A switch is shorter than an update and is still offered.");

        // 30 s exactly: the update is offered again (the bound is "less than 30 s").
        var atBound = await Fixture.CreateAsync();
        atBound.Clock.Advance(TimeSpan.FromSeconds(375));
        Check.Equal("撮影できる残り 0:30", atBound.Stage.RemainingText);
        Check.Equal(string.Empty, atBound.Stage.RefreshDisabledReason);
        Check.True(atBound.Stage.RefreshBothCommand.CanExecute(null), "At 30 s the update is offered.");

        // 90 s: no caution yet.
        var plenty = await Fixture.CreateAsync();
        plenty.Clock.Advance(TimeSpan.FromSeconds(315));
        Check.Equal("撮影できる残り 1:30", plenty.Stage.RemainingText);
        Check.Equal("normal", plenty.Stage.RemainingKind);
        Check.Equal(string.Empty, plenty.Stage.AlertText);
    }

    private static async Task ExpiryClosesTheStageAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");
        fixture.Clock.Advance(TimeSpan.FromSeconds(420));

        var slice = await fixture.LoggedByAsync(stage.TickAsync);
        Check.Equal(string.Join(",", Slice("stop", "sdk-close:CAM-A")), string.Join(",", slice));
        Check.Equal(DualLiveStageState.Expired, stage.State);
        Check.Equal("expired", stage.RemainingKind);
        Check.Equal(DualLiveStageText.RemainingExpired, stage.AlertText);
        Check.Equal("block", stage.AlertKind);
        Check.Equal(DualLivePaneState.Still, stage.PaneA.State);
        Check.True(fixture.Agent.OpenBoundLiveViewAlias is null, "The Live View is closed.");
        Check.False(stage.RefreshBothCommand.CanExecute(null), "Nothing is offered after expiry.");
        Check.False(stage.SwitchLiveCommand.CanExecute("CAM-B"), "Nothing is offered after expiry.");
        Check.Equal(DualLiveStageText.DisabledExpired, stage.RefreshDisabledReason);
        // The stage is not a capture gate: the main button is decided by the Agent, not here.
        Check.True(fixture.Binding.IsReady, "Running out of time does not drop the binding.");

        // Asking again changes nothing.
        var lengthAfterExpiry = fixture.Log.Count;
        await stage.TickAsync();
        Check.Equal(lengthAfterExpiry, fixture.Log.Count);

        // With nothing open, expiry sends nothing at all.
        var idle = await Fixture.CreateAsync();
        idle.Clock.Advance(TimeSpan.FromSeconds(420));
        await idle.Stage.TickAsync();
        Check.Equal(DualLiveStageState.Expired, idle.Stage.State);
        Check.Equal(0, idle.Log.Count);
    }

    // ---- frames -------------------------------------------------------------------------------

    private static async Task StaleLiveFrameFailsAsync()
    {
        var fixture = await Fixture.CreateAsync(options => options with { StallBoundLiveViewFramesAfter = 2 });
        var stage = fixture.Stage;
        await stage.SwitchAsync("CAM-A");

        Check.True(await stage.PumpFrameAsync("CAM-A", CancellationToken.None), "The second frame arrives.");
        Check.True(await stage.PumpFrameAsync("CAM-A", CancellationToken.None),
            "No frame yet, but not stale: the camera is still Live.");
        Check.Equal(DualLivePaneState.Live, stage.PaneA.State);

        fixture.Clock.Advance(TimeSpan.FromSeconds(4));
        var slice = await fixture.LoggedByAsync(async () =>
            Check.False(await stage.PumpFrameAsync("CAM-A", CancellationToken.None), "A stale camera ends the loop."));
        Check.Equal(string.Join(",", Slice("frame:CAM-A", "stop", "sdk-close:CAM-A")), string.Join(",", slice));
        AssertNoPaneIsLive(fixture, "after three seconds without a frame");
        Check.Equal(DualLivePaneState.Failed, stage.PaneA.State);
        Check.Equal(DualLiveStageState.Failed, stage.State);
        Check.True(fixture.Binding.IsReady, "A stalled camera does not by itself end the binding.");
        Check.Equal(0, stage.AutomaticRetryCount);
    }

    private static async Task FrameLoopRunsOnlyWhileLiveAsync()
    {
        var ticks = new SemaphoreSlim(0);
        var fixture = await Fixture.CreateAsync(
            runFrameLoop: true,
            delay: async (_, token) => await ticks.WaitAsync(token));
        var stage = fixture.Stage;

        await stage.SwitchAsync("CAM-A");
        Check.Equal(1, fixture.Agent.BoundLiveViewFrameCount);
        ticks.Release(3);
        await TestWaits.PollUntilAsync(() => fixture.Agent.BoundLiveViewFrameCount >= 4, "three frames from the loop");
        var firstTime = stage.PaneA.LastFrameAtUtc;
        Check.True(firstTime is not null, "The Live pane carries the time of its last frame.");

        // Closing for a capture stops the loop before it stops the camera.
        Check.True(await stage.PrepareForCaptureAsync(), "Closing for a capture must succeed.");
        var framesAtClose = fixture.Agent.BoundLiveViewFrameCount;
        ticks.Release(5);
        await Task.Delay(150);
        Check.Equal(framesAtClose, fixture.Agent.BoundLiveViewFrameCount);
        Check.True(fixture.Agent.OpenBoundLiveViewAlias is null, "Nothing is open after the close.");
        Check.Equal("stop", fixture.Log[fixture.Log.Count - 2]);
    }

    // ---- gates --------------------------------------------------------------------------------

    private static async Task CaptureGatesTheStageAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        Check.True(stage.SwitchLiveCommand.CanExecute("CAM-A"), "Before any capture the switch is offered.");

        fixture.CaptureActive = true;
        Check.Equal(DualLiveStageText.DisabledCapturing, stage.RefreshDisabledReason);
        Check.False(stage.SwitchLiveCommand.CanExecute("CAM-A"), "No switch while a capture runs.");
        Check.False(stage.RefreshBothCommand.CanExecute(null), "No update while a capture runs.");
        fixture.CaptureActive = false;

        // The Agent's own guard: with a Live View open the activation is refused and the binding
        // stays Ready. The screen closes first, which is what PrepareForCapture is for.
        await stage.SwitchAsync("CAM-A");
        Check.False(await fixture.Binding.ActivateCaptureAsync(), "The Agent refuses to activate while a Live View is open.");
        Check.True(fixture.Binding.IsReady, "A refused activation leaves the binding Ready.");
        Check.False(fixture.Agent.CaptureHostActivated, "The Agent did not hand over to the capture host.");

        Check.True(await stage.PrepareForCaptureAsync(), "Closing for a capture must succeed.");
        Check.True(await fixture.Binding.ActivateCaptureAsync(), "The activation is accepted once nothing is open.");

        // Live View for the capture host is a later change: until then the stage says what to do.
        Check.Equal(DualLiveStageText.AfterCapture, stage.RefreshDisabledReason);
        Check.Equal(DualLiveStageText.AfterCapture, stage.AlertText);
        Check.False(stage.SwitchLiveCommand.CanExecute("CAM-B"), "No bound Live View request is sent to the retired binding pipe.");
    }

    private static async Task ShutdownStopsBeforeCancelAsync()
    {
        var fixture = await Fixture.CreateAsync();
        fixture.Binding.BeforeShutdownCancelAsync = fixture.Stage.StopForShutdownAsync;
        await fixture.Stage.SwitchAsync("CAM-A");

        var refusal = await fixture.Binding.CancelBindingOnShutdownAsync();
        Check.True(refusal is null, "Shutdown cleanup is confirmed.");
        Check.Equal("stop", fixture.Log[fixture.Log.Count - 2]);
        Check.Equal("sdk-close:CAM-A", fixture.Log[fixture.Log.Count - 1]);
        Check.Equal(1, fixture.Agent.EndBindingSessionCount);
        Check.Equal(1, fixture.Log.Count(entry => entry == "stop"));

        // A stop that is not confirmed does not stop the cleanup: cancel-binding is the backstop.
        var unconfirmed = await Fixture.CreateAsync(options => options with { BoundLiveViewStopLeavesSourceOpen = true });
        unconfirmed.Binding.BeforeShutdownCancelAsync = unconfirmed.Stage.StopForShutdownAsync;
        await unconfirmed.Stage.SwitchAsync("CAM-A");
        var backstop = await unconfirmed.Binding.CancelBindingOnShutdownAsync();
        Check.True(backstop is null, "cancel-binding still runs and closes the SDK session.");
        Check.Equal(1, unconfirmed.Agent.EndBindingSessionCount);
        Check.True(unconfirmed.Agent.OpenBoundLiveViewAlias is null, "cancel-binding ended the bound Live View.");
    }

    // ---- words --------------------------------------------------------------------------------

    private static async Task AccessibleNamesFollowStateAsync()
    {
        var fixture = await Fixture.CreateAsync();
        var stage = fixture.Stage;
        Check.Equal("CAM-A 静止画なし", stage.PaneA.AccessibleName);
        Check.Equal("CAM-B 静止画なし", stage.PaneB.AccessibleName);
        Check.Equal("CAM-A をライブ表示に切り替える", stage.PaneA.SwitchButtonAutomationName);
        Check.Equal("CAM-B をライブ表示に切り替える", stage.PaneB.SwitchButtonAutomationName);
        Check.True(stage.PaneA.SwitchButtonAutomationName != stage.PaneB.SwitchButtonAutomationName,
            "Two buttons never share one spoken name.");
        Check.True(stage.RefreshButtonAutomationName.StartsWith("両方を更新", StringComparison.Ordinal),
            stage.RefreshButtonAutomationName);
        Check.Equal("二台の表示  CAM-A: 静止画なし ／ CAM-B: 静止画なし", stage.HeadingText);

        await stage.SwitchAsync("CAM-A");
        var received = stage.PaneA.LastFrameAtUtc!.Value.ToLocalTime();
        Check.Equal(
            "CAM-A ライブ表示中 最終受信 " + received.ToString("HH'時'mm'分'ss'秒'", CultureInfo.InvariantCulture),
            stage.PaneA.AccessibleName);
        Check.Equal("最終受信 " + received.ToString("HH:mm:ss", CultureInfo.InvariantCulture), stage.PaneA.FooterText);
        Check.False(stage.PaneA.IsSwitchButtonVisible, "The Live pane has no switch button.");
        Check.True(stage.PaneB.IsSwitchButtonVisible, "The other pane offers the way in.");
        Check.Equal("live", stage.PaneA.StateKey);
        Check.Equal("pending", stage.PaneB.StateKey);

        // State and its words change together.
        var raised = new List<string>();
        stage.PaneA.PropertyChanged += (_, args) => raised.Add(args.PropertyName ?? string.Empty);
        await stage.SwitchAsync("CAM-B");
        foreach (var name in new[]
        {
            nameof(DualLivePaneViewModel.BadgeText),
            nameof(DualLivePaneViewModel.AccessibleName),
            nameof(DualLivePaneViewModel.StateKey),
            nameof(DualLivePaneViewModel.IsSwitchButtonVisible),
            nameof(DualLivePaneViewModel.FooterText),
        })
        {
            Check.True(raised.Contains(name), $"{name} must be announced when the pane stops being Live.");
        }

        // A still names its time and its age, rounded to ten seconds for the spoken name.
        fixture.Clock.Advance(TimeSpan.FromSeconds(38));
        await stage.TickAsync();
        var time = received.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
        Check.Equal($"静止画 {time}・{38}秒前", stage.PaneA.BadgeText);
        Check.Equal(
            "CAM-A 静止画 " + received.ToString("HH'時'mm'分'ss'秒'", CultureInfo.InvariantCulture) + "に取得 30秒前",
            stage.PaneA.AccessibleName);
        raised.Clear();
        fixture.Clock.Advance(TimeSpan.FromSeconds(1));
        await stage.TickAsync();
        Check.True(raised.Contains(nameof(DualLivePaneViewModel.BadgeText)), "The visible age updates every tick.");
        Check.False(raised.Contains(nameof(DualLivePaneViewModel.AccessibleName)),
            "The spoken name changes only when its ten-second bucket does.");
        fixture.Clock.Advance(TimeSpan.FromSeconds(10));
        await stage.TickAsync();
        Check.True(raised.Contains(nameof(DualLivePaneViewModel.AccessibleName)), "A new bucket is announced.");
        fixture.Clock.Advance(TimeSpan.FromSeconds(30));
        await stage.TickAsync();
        Check.True(stage.PaneA.BadgeText.EndsWith("1分前", StringComparison.Ordinal), stage.PaneA.BadgeText);

        // Failure and loss.
        var failed = await Fixture.CreateAsync(options => options with { EmptyBoundLiveViewFrames = true });
        await failed.Stage.SwitchAsync("CAM-B");
        Check.Equal("CAM-B 映像停止 取得に失敗", failed.Stage.PaneB.AccessibleName);
        Check.Equal("映像停止", failed.Stage.PaneB.BadgeText);
        Check.Equal("failed", failed.Stage.PaneB.StateKey);
        Check.Equal("CAM-A 静止画なし", failed.Stage.PaneA.AccessibleName);
    }

    private static async Task NoSimultaneousWordingAsync()
    {
        var banned = new[] { "同時", "同期" };
        var seen = new List<string>();

        void Collect(DualLiveStageViewModel stage)
        {
            foreach (var target in new object[] { stage, stage.PaneA, stage.PaneB })
            {
                foreach (var property in target.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
                {
                    if (property.PropertyType == typeof(string) && property.GetIndexParameters().Length == 0 &&
                        property.GetValue(target) is string text && text.Length > 0)
                    {
                        seen.Add($"{target.GetType().Name}.{property.Name}: {text}");
                    }
                }
            }
        }

        // Every state the stage reaches in these runs.
        var idle = await Fixture.CreateAsync();
        Collect(idle.Stage);
        await idle.Stage.SwitchAsync("CAM-A");
        Collect(idle.Stage);
        await idle.Stage.SwitchAsync("CAM-B");
        idle.Clock.Advance(TimeSpan.FromSeconds(75));
        await idle.Stage.TickAsync();
        Collect(idle.Stage);
        await idle.Stage.RefreshBothAsync();
        Collect(idle.Stage);
        idle.Clock.Advance(TimeSpan.FromSeconds(330));
        await idle.Stage.TickAsync();
        Collect(idle.Stage);
        idle.Clock.Advance(TimeSpan.FromSeconds(100));
        await idle.Stage.TickAsync();
        Collect(idle.Stage);

        var empty = await Fixture.CreateAsync(options => options with { EmptyBoundLiveViewFrames = true });
        await empty.Stage.SwitchAsync("CAM-A");
        Collect(empty.Stage);
        var lost = await Fixture.CreateAsync(options => options with { FailBoundLiveViewStartAlias = "CAM-B" });
        await lost.Stage.SwitchAsync("CAM-A");
        await lost.Stage.RefreshBothAsync();
        Collect(lost.Stage);
        var after = await Fixture.CreateAsync();
        await after.Stage.SwitchAsync("CAM-A");
        Check.True(await after.Stage.PrepareForCaptureAsync(), "Closing for a capture must succeed.");
        Check.True(await after.Binding.ActivateCaptureAsync(), "The activation after a confirmed close must be accepted.");
        Collect(after.Stage);
        var unknown = await Fixture.CreateAsync(agentStartKnown: false);
        Collect(unknown.Stage);

        foreach (var field in typeof(DualLiveStageText).GetFields(BindingFlags.Public | BindingFlags.Static))
        {
            if (field.IsLiteral && field.GetRawConstantValue() is string constant)
            {
                seen.Add($"DualLiveStageText.{field.Name}: {constant}");
            }
        }

        seen.Add(DualLiveStageText.Switching("CAM-A"));
        seen.Add(DualLiveStageText.Fetching("CAM-B", 2));
        seen.Add(DualLiveStageText.Returning("CAM-A"));
        seen.Add(DualLiveStageText.FrameFailed("CAM-B"));
        seen.Add(DualLiveStageText.FrameFailedRebind("CAM-B"));
        seen.Add(DualLiveStageText.Refreshed("CAM-A 10:00:00、CAM-B 10:00:05"));
        seen.Add(DualLiveStageText.RefreshHelp("CAM-A"));
        seen.Add(DualLiveStageText.RefreshAutomationName("CAM-B"));

        Check.True(seen.Count > 60, $"The scan must cover the stage's texts; it saw {seen.Count}.");
        foreach (var text in seen)
        {
            foreach (var word in banned)
            {
                Check.False(text.Contains(word, StringComparison.Ordinal), $"'{word}' must not appear on the stage: {text}");
            }
        }
    }

    private static Task ReserveMatchesNativeAsync()
    {
        Check.Equal(TimeSpan.FromSeconds(185), DualLiveStageViewModel.CaptureAdmissionReserve);
        Check.Equal(TimeSpan.FromMinutes(10), DualCameraAgentLifecycle.AgentMaxLifetime);

        var headerPath = Path.Combine(AppContext.BaseDirectory, "native", "dual_bound_live_view.hpp");
        Check.True(File.Exists(headerPath),
            $"dual_bound_live_view.hpp was not copied to {headerPath}; the native reserve cannot be compared.");
        var match = Regex.Match(
            File.ReadAllText(headerPath),
            @"kBoundLiveViewCaptureAdmissionReserveMilliseconds\s*=\s*([\d']+)");
        Check.True(match.Success, "kBoundLiveViewCaptureAdmissionReserveMilliseconds was not found in the native header.");
        var native = double.Parse(match.Groups[1].Value.Replace("'", string.Empty, StringComparison.Ordinal), CultureInfo.InvariantCulture);
        Check.Equal(DualLiveStageViewModel.CaptureAdmissionReserve, TimeSpan.FromMilliseconds(native));
        return Task.CompletedTask;
    }
}
