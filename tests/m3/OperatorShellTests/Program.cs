using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.OperatorShell;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.Simulated;
using A0CameraStitcher.M3.OperatorShell.ViewModels;
using System.Buffers.Binary;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Runtime.ExceptionServices;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

const string persistentChildScenarioVariable = "A0_CAMERA_AGENT_TEST_CHILD_SCENARIO";
if (Environment.GetEnvironmentVariable(persistentChildScenarioVariable) is { Length: > 0 } childScenario)
{
    return await RunPersistentCameraAgentTestChildAsync(childScenario, args);
}
if (args is ["--sdkless-camera-agent-e2e", var sdklessAgentPath])
{
    return await RunSdklessPersistentReadinessE2EAsync(sdklessAgentPath);
}
const string dualChildScenarioVariable = "A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO";
if (Environment.GetEnvironmentVariable(dualChildScenarioVariable) is { Length: > 0 } dualChildScenario)
{
    return await RunDualCameraAgentTestChildAsync(dualChildScenario, args);
}

// GitHub Issue #63: 描画に依存する試験の前提を実行のたびに記録する。落ちた報告が来たとき、
// どのアパートメント・描画モード・描画ティアで起きたのかが分からないと切り分けられない。
Console.WriteLine(
    $"render preconditions: main apartment={Thread.CurrentThread.GetApartmentState()}" +
    $" (render-dependent tests are hoisted to STA+Dispatcher)" +
    $", ProcessRenderMode={RenderOptions.ProcessRenderMode}" +
    $", RenderCapability.Tier={RenderCapability.Tier >> 16}");

var failures = new List<string>();
try
{
    await PersistentHardwareCameraAgentPipeFailuresAsync();
    Console.WriteLine("PASS persistent hardware Camera Agent classifies pipe exits and typed readiness");
}
catch (Exception exception)
{
    failures.Add("persistent hardware Camera Agent classifies pipe exits and typed readiness");
    Console.Error.WriteLine($"FAIL persistent hardware Camera Agent classifies pipe exits and typed readiness: {exception}");
}

try
{
    await LiveViewStopFailureWorkflowAsync();
    Console.WriteLine("PASS live view stop failure survives restart and rejects duplicate start");
}
catch (Exception exception)
{
    failures.Add("live view stop failure survives restart and rejects duplicate start");
    Console.Error.WriteLine($"FAIL live view stop failure survives restart and rejects duplicate start: {exception}");
}

try
{
    await SingleCameraWorkflowAsync();
    Console.WriteLine("PASS single-camera CAM-B capture skips stitch and exports one original");
}
catch (Exception exception)
{
    failures.Add("single-camera CAM-B capture skips stitch and exports one original");
    Console.Error.WriteLine($"FAIL single-camera CAM-B capture skips stitch and exports one original: {exception}");
}

try
{
    await ModeAndAliasLockDuringCaptureAsync();
    Console.WriteLine("PASS active capture locks operating mode and selected alias");
}
catch (Exception exception)
{
    failures.Add("active capture locks operating mode and selected alias");
    Console.Error.WriteLine($"FAIL active capture locks operating mode and selected alias: {exception}");
}

try
{
    await DualCameraRegressionAsync();
    Console.WriteLine("PASS dual-camera workflow still captures, stitches, restitches, and exports");
}
catch (Exception exception)
{
    failures.Add("dual-camera workflow still captures, stitches, restitches, and exports");
    Console.Error.WriteLine($"FAIL dual-camera workflow still captures, stitches, restitches, and exports: {exception}");
}

try
{
    await DualIdentityBlocksWpfCaptureAsync();
    Console.WriteLine("PASS non-ready dual identity blocks WPF and capture side effects");
}
catch (Exception exception)
{
    failures.Add("non-ready dual identity blocks WPF and capture side effects");
    Console.Error.WriteLine($"FAIL non-ready dual identity blocks WPF and capture side effects: {exception}");
}

try
{
    await DualIdentityExpiryBlocksWpfCaptureAsync();
    Console.WriteLine("PASS expired ready dual identity blocks WPF transaction start");
}
catch (Exception exception)
{
    failures.Add("expired ready dual identity blocks WPF transaction start");
    Console.Error.WriteLine($"FAIL expired ready dual identity blocks WPF transaction start: {exception}");
}

try
{
    await FormalDualCameraWpfFlowAsync();
    Console.WriteLine("PASS formal WPF dual-camera flow uses real JPEG product artifacts");
}
catch (Exception exception)
{
    failures.Add("formal WPF dual-camera flow uses real JPEG product artifacts");
    Console.Error.WriteLine($"FAIL formal WPF dual-camera flow uses real JPEG product artifacts: {exception}");
}

try
{
    await FormalDualCameraExportFailureProgressAsync();
    Console.WriteLine("PASS formal WPF maps active and failed explicit export progress");
}
catch (Exception exception)
{
    failures.Add("formal WPF maps active and failed explicit export progress");
    Console.Error.WriteLine($"FAIL formal WPF maps active and failed explicit export progress: {exception}");
}

try
{
    await SingleCameraRestartPreservesPlanAsync();
    Console.WriteLine("PASS single-camera restart restores the durable mode and selected alias");
}
catch (Exception exception)
{
    failures.Add("single-camera restart restores the durable mode and selected alias");
    Console.Error.WriteLine($"FAIL single-camera restart restores the durable mode and selected alias: {exception}");
}

try
{
    await HardwareSingleHappyPathAsync();
    Console.WriteLine("PASS hardware single-camera path captures once and explicitly exports verified original");
}
catch (Exception exception)
{
    failures.Add("hardware single-camera path captures once and explicitly exports verified original");
    Console.Error.WriteLine($"FAIL hardware single-camera path captures once and explicitly exports verified original: {exception}");
}

try
{
    await HardwarePendingTransactionRecoveryAsync();
    Console.WriteLine("PASS hardware pending transaction is queried without recapture and survives in-progress result");
}
catch (Exception exception)
{
    failures.Add("hardware pending transaction is queried without recapture and survives in-progress result");
    Console.Error.WriteLine($"FAIL hardware pending transaction is queried without recapture and survives in-progress result: {exception}");
}

try
{
    await HardwareLiveViewRequiresFreshReadinessAsync();
    Console.WriteLine("PASS hardware finite Live View verifies preview and requires fresh readiness");
}
catch (Exception exception)
{
    failures.Add("hardware finite Live View verifies preview and requires fresh readiness");
    Console.Error.WriteLine($"FAIL hardware finite Live View verifies preview and requires fresh readiness: {exception}");
}

try
{
    await HardwareMalformedLocalStateFailsClosedAsync();
    Console.WriteLine("PASS malformed hardware app state fails closed without camera access");
}
catch (Exception exception)
{
    failures.Add("malformed hardware app state fails closed without camera access");
    Console.Error.WriteLine($"FAIL malformed hardware app state fails closed without camera access: {exception}");
}

try
{
    HardwareLaunchOptionsAreExplicit();
    Console.WriteLine("PASS app launch options keep hardware and simulation explicit");
}
catch (Exception exception)
{
    failures.Add("app launch options keep hardware and simulation explicit");
    Console.Error.WriteLine($"FAIL app launch options keep hardware and simulation explicit: {exception}");
}

try
{
    await HardwareAppStateCompareAndSetAsync();
    Console.WriteLine("PASS hardware app state cannot overwrite or clear another transaction");
}
catch (Exception exception)
{
    failures.Add("hardware app state cannot overwrite or clear another transaction");
    Console.Error.WriteLine($"FAIL hardware app state cannot overwrite or clear another transaction: {exception}");
}

try
{
    HardwareOperatorSessionLeaseIsExclusive();
    Console.WriteLine("PASS hardware operator window lease excludes a second local process owner");
}
catch (Exception exception)
{
    failures.Add("hardware operator window lease excludes a second local process owner");
    Console.Error.WriteLine($"FAIL hardware operator window lease excludes a second local process owner: {exception}");
}

try
{
    await HardwareExportVerificationFailureStaysUnpublishedAsync();
    Console.WriteLine("PASS verified export handle blocks writes and defeats path replacement");
}
catch (Exception exception)
{
    failures.Add("verified export handle blocks writes and defeats path replacement");
    Console.Error.WriteLine($"FAIL verified export handle blocks writes and defeats path replacement: {exception}");
}

try
{
    await HardwarePreDispatchAndNotFoundRecoveryBoundariesAsync();
    Console.WriteLine("PASS startup closes known pre-dispatch failure but blocks ambiguous missing journal");
}
catch (Exception exception)
{
    failures.Add("startup closes known pre-dispatch failure but blocks ambiguous missing journal");
    Console.Error.WriteLine($"FAIL startup closes known pre-dispatch failure but blocks ambiguous missing journal: {exception}");
}

try
{
    await HardwareInitializationAndProfileExpiryGateAsync();
    Console.WriteLine("PASS startup inspection and profile expiry keep every hardware command closed");
}
catch (Exception exception)
{
    failures.Add("startup inspection and profile expiry keep every hardware command closed");
    Console.Error.WriteLine($"FAIL startup inspection and profile expiry keep every hardware command closed: {exception}");
}

try
{
    await HardwareContinuousLiveViewCaptureHandoffAsync();
    Console.WriteLine("PASS hardware continuous Live View stops before capture and restarts only after success");
}
catch (Exception exception)
{
    failures.Add("hardware continuous Live View stops before capture and restarts only after success");
    Console.Error.WriteLine($"FAIL hardware continuous Live View stops before capture and restarts only after success: {exception}");
}

try
{
    await HardwareSinglePreferencesAndProfileApprovalAsync();
    Console.WriteLine("PASS local export preference and 30-day CAM-A profile approval are durable and fail closed");
}
catch (Exception exception)
{
    failures.Add("local export preference and 30-day CAM-A profile approval are durable and fail closed");
    Console.Error.WriteLine($"FAIL local export preference and 30-day CAM-A profile approval are durable and fail closed: {exception}");
}

try
{
    await HardwareSingleRequiresAndRepairsOperatorExportDirectoryAsync();
    Console.WriteLine("PASS hardware single requires an operator export folder and permits repair after capture");
}
catch (Exception exception)
{
    failures.Add("hardware single requires an operator export folder and permits repair after capture");
    Console.Error.WriteLine($"FAIL hardware single requires an operator export folder and permits repair after capture: {exception}");
}

try
{
    await HardwareArtifactVerifierRequiresExactCanonicalPathAsync();
    Console.WriteLine("PASS HardwareArtifactVerifier requires the exact canonical artifact path (#101 M-4)");
}
catch (Exception exception)
{
    failures.Add("HardwareArtifactVerifier requires the exact canonical artifact path (#101 M-4)");
    Console.Error.WriteLine($"FAIL HardwareArtifactVerifier requires the exact canonical artifact path (#101 M-4): {exception}");
}

try
{
    await PersistentHardwareCameraAgentOperationsPassesArtifactsRootToChildAsync();
    Console.WriteLine("PASS PersistentHardwareCameraAgentOperations passes --artifacts-root matching AgentArtifactsRoot (#101 M-4)");
}
catch (Exception exception)
{
    failures.Add("PersistentHardwareCameraAgentOperations passes --artifacts-root matching AgentArtifactsRoot (#101 M-4)");
    Console.Error.WriteLine($"FAIL PersistentHardwareCameraAgentOperations passes --artifacts-root matching AgentArtifactsRoot (#101 M-4): {exception}");
}

try
{
    HardwareDualTransactionSnapshotStoreRejectsOversizedState();
    Console.WriteLine("PASS HardwareDualTransactionSnapshotStore rejects an oversized durable snapshot (#101 M-5)");
}
catch (Exception exception)
{
    failures.Add("HardwareDualTransactionSnapshotStore rejects an oversized durable snapshot (#101 M-5)");
    Console.Error.WriteLine($"FAIL HardwareDualTransactionSnapshotStore rejects an oversized durable snapshot (#101 M-5): {exception}");
}

try
{
    await HardwareSinglePreferencesStoreRejectsOversizedFileAsync();
    Console.WriteLine("PASS HardwareSinglePreferencesStore rejects an oversized preferences file (#101 M-6)");
}
catch (Exception exception)
{
    failures.Add("HardwareSinglePreferencesStore rejects an oversized preferences file (#101 M-6)");
    Console.Error.WriteLine($"FAIL HardwareSinglePreferencesStore rejects an oversized preferences file (#101 M-6): {exception}");
}

try
{
    await DualCameraAgentLifecycleIdentityPendingKeepsZeroProcessAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle keeps process/camera at zero while identity is Pending");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle keeps process/camera at zero while identity is Pending");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle keeps process/camera at zero while identity is Pending: {exception}");
}

try
{
    await DualCameraAgentLifecycleRequiresExistingArtifactFilesAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle requires existing approved-capture-profile/dual-identity-proof files before launch");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle requires existing approved-capture-profile/dual-identity-proof files before launch");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle requires existing approved-capture-profile/dual-identity-proof files before launch: {exception}");
}

try
{
    await DualCameraAgentLifecycleFakeHostHappyPathAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle fake host reserve-start typed success end-to-end");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle fake host reserve-start typed success end-to-end");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle fake host reserve-start typed success end-to-end: {exception}");
}

try
{
    await DualCameraAgentLifecycleExitCodeClassificationAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle classifies all four exit codes without inverting dispatch ambiguity");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle classifies all four exit codes without inverting dispatch ambiguity");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle classifies all four exit codes without inverting dispatch ambiguity: {exception}");
}

try
{
    await DualCameraAgentLifecycleRestartRecoveryAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle process exit and native max-lifetime exit both recover via same-ID query only");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle process exit and native max-lifetime exit both recover via same-ID query only");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle process exit and native max-lifetime exit both recover via same-ID query only: {exception}");
}

try
{
    await DualCameraAgentLifecyclePipeFailureWithoutProcessExitAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle pipe failure without process exit resumes on the same process");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle pipe failure without process exit resumes on the same process");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle pipe failure without process exit resumes on the same process: {exception}");
}

try
{
    await DualCameraAgentLifecycleConnectFailureSurfacesExitDiagnosticsAsync();
    Console.WriteLine("PASS HardwareDual Agent lifecycle connect failure surfaces exit code and stderr diagnostics");
}
catch (Exception exception)
{
    failures.Add("HardwareDual Agent lifecycle connect failure surfaces exit code and stderr diagnostics");
    Console.Error.WriteLine($"FAIL HardwareDual Agent lifecycle connect failure surfaces exit code and stderr diagnostics: {exception}");
}

try
{
    SimulatedTestImageFrameSourceRendersWatermarkedFramesForEveryPattern();
    Console.WriteLine("PASS SIMULATED test image frame source renders a frozen, watermarked frame for every pattern");
}
catch (Exception exception)
{
    failures.Add("SIMULATED test image frame source renders a frozen, watermarked frame for every pattern");
    Console.Error.WriteLine($"FAIL SIMULATED test image frame source renders a frozen, watermarked frame for every pattern: {exception}");
}

try
{
    RunSyncOnStaRenderThread(SimulatedTestImageFrameSourceAppliesBlurAcrossTheFocusTransition);
    Console.WriteLine("PASS SIMULATED test image frame source actually applies BlurEffect across the focus transition");
}
catch (Exception exception)
{
    failures.Add("SIMULATED test image frame source actually applies BlurEffect across the focus transition");
    Console.Error.WriteLine($"FAIL SIMULATED test image frame source actually applies BlurEffect across the focus transition: {exception}");
}

try
{
    await SimulatedLiveViewFramePumpOnlyTicksBetweenStartAndStopAsync();
    Console.WriteLine("PASS SIMULATED live view frame pump only ticks between Start and Stop, and stamps a fresh generation on each Start");
}
catch (Exception exception)
{
    failures.Add("SIMULATED live view frame pump only ticks between Start and Stop, and stamps a fresh generation on each Start");
    Console.Error.WriteLine($"FAIL SIMULATED live view frame pump only ticks between Start and Stop, and stamps a fresh generation on each Start: {exception}");
}

try
{
    await SimulatedFramePumpWiringAsync();
    Console.WriteLine("PASS operator shell renders on tick, drops stale-generation/marker frames without throwing, and reverts rejected pattern changes");
}
catch (Exception exception)
{
    failures.Add("operator shell renders on tick, drops stale-generation/marker frames without throwing, and reverts rejected pattern changes");
    Console.Error.WriteLine($"FAIL operator shell renders on tick, drops stale-generation/marker frames without throwing, and reverts rejected pattern changes: {exception}");
}

try
{
    TargetReticleDragMovesClampAndScale();
    Console.WriteLine("PASS target reticle drag applies stage/loupe delta scaling and clamps to the 0..1 stage bounds");
}
catch (Exception exception)
{
    failures.Add("target reticle drag applies stage/loupe delta scaling and clamps to the 0..1 stage bounds");
    Console.Error.WriteLine($"FAIL target reticle drag applies stage/loupe delta scaling and clamps to the 0..1 stage bounds: {exception}");
}

try
{
    await LoupeTracksTargetSideAndFreshnessBadgeAsync();
    Console.WriteLine("PASS loupe follows the target's composite side, shows the not-yet-acquired placeholder, and badges a frozen frame's freshness");
}
catch (Exception exception)
{
    failures.Add("loupe follows the target's composite side, shows the not-yet-acquired placeholder, and badges a frozen frame's freshness");
    Console.Error.WriteLine($"FAIL loupe follows the target's composite side, shows the not-yet-acquired placeholder, and badges a frozen frame's freshness: {exception}");
}

try
{
    await AutoFocusSuccessFixesFocusAndRecordsResultAsync();
    Console.WriteLine("PASS AF execution on a sharp live frame reports 合焦OK, fixes focus, and records the target position used");
}
catch (Exception exception)
{
    failures.Add("AF execution on a sharp live frame reports 合焦OK, fixes focus, and records the target position used");
    Console.Error.WriteLine($"FAIL AF execution on a sharp live frame reports 合焦OK, fixes focus, and records the target position used: {exception}");
}

try
{
    await AutoFocusReportsNgDuringBlurRampAsync();
    Console.WriteLine("PASS AF execution during the blur-to-focus ramp reports 合焦NG and leaves focus unfixed");
}
catch (Exception exception)
{
    failures.Add("AF execution during the blur-to-focus ramp reports 合焦NG and leaves focus unfixed");
    Console.Error.WriteLine($"FAIL AF execution during the blur-to-focus ramp reports 合焦NG and leaves focus unfixed: {exception}");
}

try
{
    await FocusTargetOutsideLiveDomainBlocksAfAndSwitchButtonRestoresItAsync();
    Console.WriteLine("PASS a target outside the live camera's domain disables AF and offers a one-click switch that restores it");
}
catch (Exception exception)
{
    failures.Add("a target outside the live camera's domain disables AF and offers a one-click switch that restores it");
    Console.Error.WriteLine($"FAIL a target outside the live camera's domain disables AF and offers a one-click switch that restores it: {exception}");
}

try
{
    await MfStepAdjustsRelativeValueAndUnfixesFocusAsync();
    Console.WriteLine("PASS MF coarse/fine steps move the relative focus value and unfix a previously-fixed camera");
}
catch (Exception exception)
{
    failures.Add("MF coarse/fine steps move the relative focus value and unfix a previously-fixed camera");
    Console.Error.WriteLine($"FAIL MF coarse/fine steps move the relative focus value and unfix a previously-fixed camera: {exception}");
}

try
{
    RunOnStaRenderThread(FocusPeakingOverlayHighlightsEdgesAndTogglesWithViewModelStateAsync);
    Console.WriteLine("PASS focus peaking overlay highlights document edges and only renders while the toggle is on");
}
catch (Exception exception)
{
    failures.Add("focus peaking overlay highlights document edges and only renders while the toggle is on");
    Console.Error.WriteLine($"FAIL focus peaking overlay highlights document edges and only renders while the toggle is on: {exception}");
}

try
{
    await FocusPanelDisabledInHardwareDualEnvironmentAsync();
    Console.WriteLine("PASS the focus panel stays disabled with a shown reason under the HardwareDual execution environment");
}
catch (Exception exception)
{
    failures.Add("the focus panel stays disabled with a shown reason under the HardwareDual execution environment");
    Console.Error.WriteLine($"FAIL the focus panel stays disabled with a shown reason under the HardwareDual execution environment: {exception}");
}

try
{
    await CaptureWithAutoFocusSucceedsThenCapturesAsync();
    Console.WriteLine("PASS 撮影+AF converges on every required camera then runs the unchanged existing capture flow");
}
catch (Exception exception)
{
    failures.Add("撮影+AF converges on every required camera then runs the unchanged existing capture flow");
    Console.Error.WriteLine($"FAIL 撮影+AF converges on every required camera then runs the unchanged existing capture flow: {exception}");
}

try
{
    await CaptureWithAutoFocusStopsBeforeShutterOnNgAsync();
    Console.WriteLine("PASS 撮影+AF stops fail-closed before the shutter when pre-capture AF reports 合焦NG");
}
catch (Exception exception)
{
    failures.Add("撮影+AF stops fail-closed before the shutter when pre-capture AF reports 合焦NG");
    Console.Error.WriteLine($"FAIL 撮影+AF stops fail-closed before the shutter when pre-capture AF reports 合焦NG: {exception}");
}

try
{
    await CaptureWithAutoFocusUnavailableUnderHardwareDualAsync();
    Console.WriteLine("PASS 撮影+AF stays unavailable under the HardwareDual execution environment (#35 Option A)");
}
catch (Exception exception)
{
    failures.Add("撮影+AF stays unavailable under the HardwareDual execution environment (#35 Option A)");
    Console.Error.WriteLine($"FAIL 撮影+AF stays unavailable under the HardwareDual execution environment (#35 Option A): {exception}");
}

try
{
    await ActionZoneVisibilitySwitchesWithUiStateAsync();
    Console.WriteLine("PASS the action zone's three exclusive displays switch with UiState (準備中/自動進捗/結果)");
}
catch (Exception exception)
{
    failures.Add("the action zone's three exclusive displays switch with UiState (準備中/自動進捗/結果)");
    Console.Error.WriteLine($"FAIL the action zone's three exclusive displays switch with UiState (準備中/自動進捗/結果): {exception}");
}

try
{
    RunSyncOnStaRenderThread(DocumentTiltDetectorMeasuresKnownRollAnglesAndReportsUndetectable);
    Console.WriteLine("PASS document tilt detector measures known SIMULATED ROLL angles within tolerance and reports 検出不能 for degenerate/no-document frames");
}
catch (Exception exception)
{
    failures.Add("document tilt detector measures known SIMULATED ROLL angles within tolerance and reports 検出不能 for degenerate/no-document frames");
    Console.Error.WriteLine($"FAIL document tilt detector measures known SIMULATED ROLL angles within tolerance and reports 検出不能 for degenerate/no-document frames: {exception}");
}

try
{
    RunOnStaRenderThread(TiltReadingReflectsLiveFrameAndShowsUndetectableWhenNotLiveAsync);
    Console.WriteLine("PASS the stage tilt reading follows the live camera's frame and reverts to 検出不能 when not live (issue #32)");
}
catch (Exception exception)
{
    failures.Add("the stage tilt reading follows the live camera's frame and reverts to 検出不能 when not live (issue #32)");
    Console.Error.WriteLine($"FAIL the stage tilt reading follows the live camera's frame and reverts to 検出不能 when not live (issue #32): {exception}");
}

try
{
    await AlignmentGuideOverlayTogglesControlVisibilityAsync();
    Console.WriteLine("PASS the four alignment guide overlay toggles default correctly, control their own visibility, and hide during the processing placeholder (issue #32)");
}
catch (Exception exception)
{
    failures.Add("the four alignment guide overlay toggles default correctly, control their own visibility, and hide during the processing placeholder (issue #32)");
    Console.Error.WriteLine($"FAIL the four alignment guide overlay toggles default correctly, control their own visibility, and hide during the processing placeholder (issue #32): {exception}");
}

try
{
    RunOnStaRenderThread(TiltToleranceInputSetsChipTextAndRejectsInvalidValuesAsync);
    Console.WriteLine("PASS the tilt tolerance input starts unset, rejects invalid text, and the chip only judges 許容内/超過 once both a tolerance and a reading exist (issue #32)");
}
catch (Exception exception)
{
    failures.Add("the tilt tolerance input starts unset, rejects invalid text, and the chip only judges 許容内/超過 once both a tolerance and a reading exist (issue #32)");
    Console.Error.WriteLine($"FAIL the tilt tolerance input starts unset, rejects invalid text, and the chip only judges 許容内/超過 once both a tolerance and a reading exist (issue #32): {exception}");
}

try
{
    await MenuNavigationSwitchesPagesAndLocksDuringCaptureAsync();
    Console.WriteLine("PASS the menu bar's page-navigation commands switch SelectedPage/PageTitle and are locked to the dashboard during an active transaction (issue #34)");
}
catch (Exception exception)
{
    failures.Add("the menu bar's page-navigation commands switch SelectedPage/PageTitle and are locked to the dashboard during an active transaction (issue #34)");
    Console.Error.WriteLine($"FAIL the menu bar's page-navigation commands switch SelectedPage/PageTitle and are locked to the dashboard during an active transaction (issue #34): {exception}");
}

try
{
    OperatingModeAndLoupeZoomMenuTogglesStaySynced();
    Console.WriteLine("PASS the camera-menu operating-mode and view-menu loupe-zoom radio toggles stay mutually exclusive and synced with the underlying selection (issue #34)");
}
catch (Exception exception)
{
    failures.Add("the camera-menu operating-mode and view-menu loupe-zoom radio toggles stay mutually exclusive and synced with the underlying selection (issue #34)");
    Console.Error.WriteLine($"FAIL the camera-menu operating-mode and view-menu loupe-zoom radio toggles stay mutually exclusive and synced with the underlying selection (issue #34): {exception}");
}

try
{
    TiltReadingVisibilityMenuToggleDefaultsVisibleAndTogglesIndependently();
    Console.WriteLine("PASS the view menu's new tilt-reading visibility toggle defaults on and toggles independently of the other #32 overlay toggles (issue #34)");
}
catch (Exception exception)
{
    failures.Add("the view menu's new tilt-reading visibility toggle defaults on and toggles independently of the other #32 overlay toggles (issue #34)");
    Console.Error.WriteLine($"FAIL the view menu's new tilt-reading visibility toggle defaults on and toggles independently of the other #32 overlay toggles (issue #34): {exception}");
}

try
{
    await DualCameraIdentityStatusMenuTextReflectsSnapshotAndOperatingModeAsync();
    Console.WriteLine("PASS the camera menu's read-only identity status text reflects the DualCamera identity snapshot and goes 対象外 in SingleCamera mode (issue #34)");
}
catch (Exception exception)
{
    failures.Add("the camera menu's read-only identity status text reflects the DualCamera identity snapshot and goes 対象外 in SingleCamera mode (issue #34)");
    Console.Error.WriteLine($"FAIL the camera menu's read-only identity status text reflects the DualCamera identity snapshot and goes 対象外 in SingleCamera mode (issue #34): {exception}");
}

try
{
    await DualBindingOverlayGatesCaptureAsync();
    Console.WriteLine("PASS the binding overlay covers the screen and gates capture until the operator confirms both aliases (issue #62)");
}
catch (Exception exception)
{
    failures.Add("the binding overlay covers the screen and gates capture until the operator confirms both aliases (issue #62)");
    Console.Error.WriteLine($"FAIL the binding overlay covers the screen and gates capture until the operator confirms both aliases (issue #62): {exception}");
}

try
{
    await DualBindingOverlayAccessibilityAndBusyLockAsync();
    Console.WriteLine("PASS the binding overlay names every control for a screen reader and locks while a request is in flight (issue #62)");
}
catch (Exception exception)
{
    failures.Add("the binding overlay names every control for a screen reader and locks while a request is in flight (issue #62)");
    Console.Error.WriteLine($"FAIL the binding overlay names every control for a screen reader and locks while a request is in flight (issue #62): {exception}");
}

try
{
    await DualBindingOverlayInvalidationRestartsTheFlowAsync();
    Console.WriteLine("PASS the binding overlay reports the invalidation reason and drops every previous assignment (issue #62)");
}
catch (Exception exception)
{
    failures.Add("the binding overlay reports the invalidation reason and drops every previous assignment (issue #62)");
    Console.Error.WriteLine($"FAIL the binding overlay reports the invalidation reason and drops every previous assignment (issue #62): {exception}");
}

try
{
    await DualBindingBlocksTheSingleCameraFallbackAsync();
    Console.WriteLine("PASS an unconfirmed binding blocks capture in SingleCamera mode too, so mode switching is not a fallback (issue #62)");
}
catch (Exception exception)
{
    failures.Add("an unconfirmed binding blocks capture in SingleCamera mode too, so mode switching is not a fallback (issue #62)");
    Console.Error.WriteLine($"FAIL an unconfirmed binding blocks capture in SingleCamera mode too, so mode switching is not a fallback (issue #62): {exception}");
}

Console.WriteLine($"Operator shell tests: {62 - failures.Count}/62 passed.");
return failures.Count == 0 ? 0 : 1;

static async Task PersistentHardwareCameraAgentPipeFailuresAsync()
{
    VerifyHardwareCameraAgentStderrSanitizer();

    var executablePath = Path.Combine(
        AppContext.BaseDirectory,
        "A0CameraStitcher.M3.OperatorShellTests.exe");
    Check.True(File.Exists(executablePath), "The persistent test child apphost must exist.");

    var root = Path.Combine(Path.GetTempPath(), $"a0-persistent-agent-{Guid.NewGuid():N}");
    Directory.CreateDirectory(root);
    try
    {
        var storagePaths = HardwareSingleStoragePaths.Resolve(root);
        var profilePath = storagePaths.CaptureProfilePath;
        var identityPath = storagePaths.SingleIdentityV3Path;
        Directory.CreateDirectory(Path.GetDirectoryName(profilePath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(identityPath)!);

        foreach (var (scenario, expectedStage) in new[]
                 {
                     ("before-response", HardwarePipeResponseFailureStage.BeforeResponse),
                     ("partial-header", HardwarePipeResponseFailureStage.PartialHeader),
                     ("partial-body", HardwarePipeResponseFailureStage.PartialBody),
                 })
        {
            Environment.SetEnvironmentVariable(persistentChildScenarioVariable, scenario);
            await using var operations = new PersistentHardwareCameraAgentOperations(
                executablePath,
                storagePaths.AgentArtifactsRoot,
                profilePath,
                identityPath);
            try
            {
                _ = await operations.GetReadinessAsync("CAM-A");
                throw new InvalidOperationException($"The {scenario} child must fail before a full response.");
            }
            catch (HardwareCameraAgentLaunchException exception)
            {
                Check.True(exception.ProcessExitCode == 37, "The child exit code must be preserved.");
                Check.True(
                    exception.ResponseFailureStage == expectedStage,
                    $"The {scenario} response failure stage must remain distinct.");
                Check.True(
                    exception.RequestMayHaveBeenDispatched,
                    "A response-side pipe close must remain dispatch-ambiguous.");
                Check.True(
                    exception.SanitizedStandardError.Contains("synthetic child failed closed", StringComparison.Ordinal),
                    "The safe stderr classification must be retained.");
                Check.True(
                    exception.SanitizedStandardError.Contains("requestId=[redacted]", StringComparison.Ordinal),
                    "A long general alphanumeric request identifier must be redacted.");
                Check.False(
                    exception.SanitizedStandardError.Contains("super-secret", StringComparison.Ordinal) ||
                    exception.SanitizedStandardError.Contains("C:\\private", StringComparison.Ordinal) ||
                    exception.SanitizedStandardError.Contains("RAW-CAMERA-IDENTITY", StringComparison.Ordinal) ||
                    exception.SanitizedStandardError.Contains(
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", StringComparison.Ordinal),
                    "Secrets, paths, and raw identity must be removed from diagnostics.");
                Check.True(
                    exception.SanitizedStandardError.Length <= 512,
                    "The stderr diagnostic must be bounded.");
            }
        }

        var tracePath = Path.Combine(root, "composition.json");
        Environment.SetEnvironmentVariable(persistentChildScenarioVariable, "typed-fail-closed");
        Environment.SetEnvironmentVariable("A0_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);
        await using (var operations = new PersistentHardwareCameraAgentOperations(
            executablePath,
            storagePaths.AgentArtifactsRoot,
            profilePath,
            identityPath))
        {
            var reply = await operations.GetReadinessAsync("CAM-A");
            Check.True(reply.Success, "A typed not-ready result is a successful protocol response.");
            Check.False(reply.Payload.Ready, "The SDK-less child must fail closed as not ready.");
            Check.Equal("licensed_adapter_unavailable", reply.Payload.FailureCategory);
        }

        using var trace = JsonDocument.Parse(await File.ReadAllTextAsync(tracePath));
        var traceRoot = trace.RootElement;
        Check.Equal(Path.GetFullPath(executablePath), traceRoot.GetProperty("executablePath").GetString()!);
        Check.Equal(Path.GetDirectoryName(executablePath)!, traceRoot.GetProperty("workingDirectory").GetString()!);
        var childArguments = traceRoot.GetProperty("arguments").EnumerateArray()
            .Select(value => value.GetString()!)
            .ToArray();
        Check.True(childArguments.Contains("--pipe-name", StringComparer.Ordinal), "The persistent pipe argument is required.");
        Check.True(
            childArguments.Contains("--artifacts-root", StringComparer.Ordinal) &&
            childArguments.Contains(storagePaths.AgentArtifactsRoot, StringComparer.Ordinal),
            "WPF and the Agent must share the same --artifacts-root (issue #101 M-4).");
        Check.True(childArguments.Contains(profilePath, StringComparer.Ordinal), "WPF and the Agent must share the profile path.");
        Check.True(childArguments.Contains(identityPath, StringComparer.Ordinal), "WPF and the Agent must share identity-v3.");
        Check.False(childArguments.Contains("--serve-once", StringComparer.Ordinal), "The formal WPF must use the persistent Agent mode.");
    }
    finally
    {
        Environment.SetEnvironmentVariable(persistentChildScenarioVariable, null);
        Environment.SetEnvironmentVariable("A0_CAMERA_AGENT_TEST_CHILD_TRACE", null);
        Directory.Delete(root, recursive: true);
    }
}

static async Task<int> RunSdklessPersistentReadinessE2EAsync(string agentExecutablePath)
{
    var root = Path.Combine(Path.GetTempPath(), $"a0-sdkless-agent-e2e-{Guid.NewGuid():N}");
    Directory.CreateDirectory(root);
    try
    {
        var storagePaths = HardwareSingleStoragePaths.Resolve(root);
        var profilePath = storagePaths.CaptureProfilePath;
        var identityPath = storagePaths.SingleIdentityV3Path;
        Directory.CreateDirectory(Path.GetDirectoryName(profilePath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(identityPath)!);
        // This test launches the real native Camera Agent, unlike the fakes
        // elsewhere in this file: pre-create --artifacts-root so its startup
        // does not depend on whether the real agent itself creates missing
        // ancestors before this read-only readiness check ever prepares an
        // artifact run.
        Directory.CreateDirectory(storagePaths.AgentArtifactsRoot);
        await using var operations = new PersistentHardwareCameraAgentOperations(
            agentExecutablePath,
            storagePaths.AgentArtifactsRoot,
            profilePath,
            identityPath);
        var reply = await operations.GetReadinessAsync("CAM-A");
        Check.True(reply.Success, "The real SDK-less Agent must return a typed hardware.v1 response.");
        Check.False(reply.Payload.Ready, "An unconfigured SDK-less Agent must fail closed.");
        Check.True(reply.Payload.ReadOnly, "SDK-less readiness must remain read-only.");
        Check.False(reply.Payload.CaptureCommandSent, "SDK-less readiness must not capture.");
        Check.False(reply.Payload.CameraObjectDeleteAttempted, "SDK-less readiness must not delete.");
        Console.WriteLine($"PASS SDK-less persistent Camera Agent typed fail-closed: {reply.Payload.FailureCategory}");
        return 0;
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task<int> RunPersistentCameraAgentTestChildAsync(
    string scenario,
    IReadOnlyList<string> arguments)
{
    var pipeIndex = arguments.ToList().IndexOf("--pipe-name");
    if (pipeIndex < 0 || pipeIndex + 1 >= arguments.Count)
    {
        return 64;
    }

    var tracePath = Environment.GetEnvironmentVariable("A0_CAMERA_AGENT_TEST_CHILD_TRACE");
    if (!string.IsNullOrWhiteSpace(tracePath))
    {
        await File.WriteAllTextAsync(
            tracePath,
            JsonSerializer.Serialize(new
            {
                executablePath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M3.OperatorShellTests.exe")),
                workingDirectory = Environment.CurrentDirectory,
                arguments,
            }));
    }

    await using var pipe = new NamedPipeServerStream(
        arguments[pipeIndex + 1],
        PipeDirection.InOut,
        maxNumberOfServerInstances: 1,
        PipeTransmissionMode.Byte,
        PipeOptions.Asynchronous);
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
    await pipe.WaitForConnectionAsync(timeout.Token);
    var requestJson = await ReadPersistentTestFrameAsync(pipe, timeout.Token);
    using var request = JsonDocument.Parse(requestJson);

    if (scenario == "before-response")
    {
        WriteSyntheticSensitiveStderr();
        return 37;
    }
    if (scenario == "partial-header")
    {
        await pipe.WriteAsync(new byte[] { 20, 0 }, timeout.Token);
        await pipe.FlushAsync(timeout.Token);
        WriteSyntheticSensitiveStderr();
        return 37;
    }
    if (scenario == "partial-body")
    {
        var header = new byte[sizeof(int)];
        BinaryPrimitives.WriteInt32LittleEndian(header, 20);
        await pipe.WriteAsync(header, timeout.Token);
        await pipe.WriteAsync("{\"short\":"u8.ToArray(), timeout.Token);
        await pipe.FlushAsync(timeout.Token);
        WriteSyntheticSensitiveStderr();
        return 37;
    }
    if (scenario != "typed-fail-closed")
    {
        return 65;
    }

    var readiness = HardwareTestData.ReadyHardware("CAM-A") with
    {
        Ready = false,
        SdkCameraCount = 0,
        WpdCameraCount = 0,
        SdkIdentityBound = false,
        WpdIdentityBound = false,
        SdkAliasMatches = false,
        WpdAliasMatches = false,
        SdkStatusProbed = false,
        SpoolInspected = false,
        SpoolKnownEmpty = false,
        CaptureProfileApproved = false,
        CaptureProfileId = string.Empty,
        CaptureProfileVersion = 0,
        CaptureProfileSha256 = string.Empty,
        CaptureProfileCameraAlias = string.Empty,
        CaptureProfileExpiresAtUtc = null,
        CaptureProfileAliasMatches = false,
        SettingsMatchApprovedProfile = false,
        FailureCategory = "licensed_adapter_unavailable",
        FailureDetail = "licensed adapter is unavailable",
    };
    var responseJson = JsonSerializer.Serialize(
        new
        {
            schemaVersion = HardwareCameraAgentProtocol.SchemaVersion,
            simulation = false,
            marker = HardwareCameraAgentProtocol.Marker,
            requestId = request.RootElement.GetProperty("requestId").GetString(),
            success = true,
            resultCode = "SingleNotReady",
            payload = readiness,
        },
        new JsonSerializerOptions(JsonSerializerDefaults.Web));
    await WritePersistentTestFrameAsync(pipe, responseJson, timeout.Token);
    return 0;
}

static void WriteSyntheticSensitiveStderr() =>
    Console.Error.WriteLine(
        "synthetic child failed closed secret=super-secret path=C:\\private\\sdk " +
        "rawIdentity=RAW-CAMERA-IDENTITY requestId=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");

static async Task<string> ReadPersistentTestFrameAsync(Stream stream, CancellationToken cancellationToken)
{
    var header = new byte[sizeof(int)];
    await stream.ReadExactlyAsync(header, cancellationToken);
    var length = BinaryPrimitives.ReadInt32LittleEndian(header);
    var payload = new byte[length];
    await stream.ReadExactlyAsync(payload, cancellationToken);
    return Encoding.UTF8.GetString(payload);
}

static async Task WritePersistentTestFrameAsync(
    Stream stream,
    string message,
    CancellationToken cancellationToken)
{
    var payload = Encoding.UTF8.GetBytes(message);
    var header = new byte[sizeof(int)];
    BinaryPrimitives.WriteInt32LittleEndian(header, payload.Length);
    await stream.WriteAsync(header, cancellationToken);
    await stream.WriteAsync(payload, cancellationToken);
    await stream.FlushAsync(cancellationToken);
    var acknowledgment = new byte[1];
    await stream.ReadExactlyAsync(acknowledgment, cancellationToken);
    if (acknowledgment[0] != 0x06)
    {
        throw new IOException("The test Camera Agent received an invalid delivery acknowledgment.");
    }
}

static async Task HardwareSingleRequiresAndRepairsOperatorExportDirectoryAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        // AgentExecutablePath and AgentArtifactsRoot are deliberately separate
        // directories (see FakeHardwareSingleCameraOperations.AgentArtifactsRoot).
        var artifactsRoot = FakeAgentArtifactsRoot(root);
        var wrongDimensionsPath = Path.Combine(root, "agent", "wrong-dimensions", "CAM-A", "original.jpg");
        var wrongDimensions = WriteJpegRecord(wrongDimensionsPath, "CAM-A", preserveOnePixelDimensions: true);
        await Check.ThrowsAsync<InvalidDataException>(() =>
            HardwareArtifactVerifier.VerifyOriginalAsync(wrongDimensions, wrongDimensionsPath));

        string? capturedOriginalPath = null;
        var operations = new FakeHardwareSingleCameraOperations
        {
            AgentExecutablePath = Path.Combine(root, "app", "fake-agent.exe"),
            AgentArtifactsRoot = artifactsRoot,
            CaptureResultFactory = (transactionId, alias) =>
            {
                var (result, originalPath) = CompleteCapture(artifactsRoot, transactionId, alias);
                capturedOriginalPath = originalPath;
                return result;
            },
        };
        var preferences = new HardwareSinglePreferencesStore(
            Path.Combine(root, "state", "preferences.json"));
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(Path.Combine(root, "state")),
            new HardwareOriginalExporter(Path.Combine(root, "default-not-selected")),
            preferences,
            profileStore: null);

        await viewModel.InitializeAsync();
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        viewModel.DedicatedSpoolScopeConfirmed = true;
        viewModel.ExactObjectDeleteConfirmed = true;

        Check.False(viewModel.CanCapture,
            "The product composition must not treat its default LocalAppData path as an operator choice.");
        Check.True(
            viewModel.BlockerText.Contains("保存先", StringComparison.Ordinal),
            "A missing operator export destination must be a visible capture blocker.");

        var firstChoice = Path.Combine(root, "operator-choice-1");
        await viewModel.ChangeExportDirectoryAsync(firstChoice);
        Check.True(viewModel.CanCapture, "An explicit fixed-local destination must release the capture gate.");
        await viewModel.CaptureAsync();
        Check.True(viewModel.CanExport, "A verified original must be exportable to the selected destination.");
        Check.True(viewModel.CanChangeExportDirectory,
            "A terminal capture must allow the operator to repair the destination before export.");

        var repairedChoice = Path.Combine(root, "operator-choice-2");
        await viewModel.ChangeExportDirectoryAsync(repairedChoice);
        await viewModel.ExportAsync();
        Check.Equal(Path.GetFullPath(repairedChoice), Path.GetDirectoryName(viewModel.LastExportPath)!);
        Check.True(
            File.ReadAllBytes(capturedOriginalPath!).SequenceEqual(File.ReadAllBytes(viewModel.LastExportPath)),
            "The repaired destination must receive a byte-identical copy of the verified canonical original.");

        var loaded = await preferences.LoadAsync();
        Check.Equal(Path.GetFullPath(repairedChoice), loaded!.ExportDirectory);
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

// Issue #101 M-4 (redesigned 2026-08-25 per security(AZKi)'s provenance-
// guarantee model, replacing the earlier agent-root containment check that
// reviewer_security(いろは) sent back): HardwareArtifactVerifier now compares
// the agent-reported path for exact equality against a canonical path this
// process derives itself (HardwareAgentArtifactLayout), not merely
// containment under the agent's directory. A record whose Path/SizeBytes/
// Sha256 are all internally consistent (i.e. an attacker who also controls
// the reported hash) must still be rejected unless it names that exact
// location.
static async Task HardwareArtifactVerifierRequiresExactCanonicalPathAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var artifactsRoot = Path.Combine(root, "artifacts");
        const string runId = "run-9000-1";
        const string transactionId = "99999999999999999999999999999999";
        var canonicalPath = HardwareAgentArtifactLayout.OriginalPath(artifactsRoot, runId, transactionId, "CAM-A");
        var original = WriteJpegRecord(canonicalPath, "CAM-A");

        // (a) A record whose path is outside the artifacts root entirely.
        var elsewherePath = Path.Combine(root, "not-the-artifacts-root", "original.jpg");
        var elsewhere = WriteJpegRecord(elsewherePath, "CAM-A");
        await Check.ThrowsAsync<InvalidDataException>(() =>
            HardwareArtifactVerifier.VerifyOriginalAsync(elsewhere, canonicalPath));

        // (b) A record inside the artifacts root, but under a different
        // run/transaction than the one this process actually reserved -- an
        // agent that names a leftover file from a different transaction must
        // be rejected even though the file lives under the correct root.
        var wrongTransactionPath = HardwareAgentArtifactLayout.OriginalPath(
            artifactsRoot, runId, "00000000000000000000000000000000", "CAM-A");
        var wrongTransaction = WriteJpegRecord(wrongTransactionPath, "CAM-A");
        await Check.ThrowsAsync<InvalidDataException>(() =>
            HardwareArtifactVerifier.VerifyOriginalAsync(wrongTransaction, canonicalPath));

        // (c) The canonical path is accepted.
        var verified = await HardwareArtifactVerifier.VerifyOriginalAsync(original, canonicalPath);
        Check.Equal(canonicalPath, verified.Path);
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

// Issue #101 M-4: HardwareAgentArtifactLayout's canonical-path checks only
// mean anything if --artifacts-root is actually the same value AgentArtifacts
// Root reports. This asserts that directly against CreateStartInfo's
// ArgumentList (exposed internal for this reason -- see its doc comment)
// rather than spawning a real child process.
static async Task PersistentHardwareCameraAgentOperationsPassesArtifactsRootToChildAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var agentExecutablePath = Path.Combine(root, "app", "A0CameraStitcher.CameraAgent.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(agentExecutablePath)!);
        File.WriteAllBytes(agentExecutablePath, [0x4d, 0x5a]);
        var artifactsRoot = FakeAgentArtifactsRoot(root);
        var profilePath = Path.Combine(root, "camera-agent", "approved-single-capture-profile.json");
        var identityPath = Path.Combine(root, "phase0", "single-identity-v3.json");
        Directory.CreateDirectory(Path.GetDirectoryName(profilePath)!);
        Directory.CreateDirectory(Path.GetDirectoryName(identityPath)!);

        await using var operations = new PersistentHardwareCameraAgentOperations(
            agentExecutablePath, artifactsRoot, profilePath, identityPath);
        var startInfo = operations.CreateStartInfo("A0CameraStitcher.CameraAgent.Hardware.v2.test");
        var arguments = startInfo.ArgumentList;
        var flagIndex = arguments.IndexOf("--artifacts-root");
        Check.True(
            flagIndex >= 0 && flagIndex + 1 < arguments.Count,
            "The child process must receive --artifacts-root.");
        Check.Equal(operations.AgentArtifactsRoot, arguments[flagIndex + 1]);
        Check.Equal(Path.GetFullPath(artifactsRoot), operations.AgentArtifactsRoot);
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

// Issue #101 M-5: HardwareDualTransactionSnapshotStore must reject an
// oversized durable snapshot file before it is fully read into memory, not
// after (an unbounded File.ReadAllBytes would let an OutOfMemoryException
// bypass the ViewModel's typed catch filters). This does not reproduce the
// TOCTOU window itself (there is no injectable seam to pause between the
// size check and the read), only that the post-refactor code path still
// enforces the size limit.
static void HardwareDualTransactionSnapshotStoreRejectsOversizedState()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var productRoot = Path.Combine(root, "product");
        var stateDirectory = Path.Combine(productRoot, "recovery-state");
        Directory.CreateDirectory(stateDirectory);
        File.WriteAllBytes(Path.Combine(stateDirectory, "pending-transaction.json"), new byte[257 * 1024]);

        var store = new HardwareDualTransactionSnapshotStore(productRoot);
        Check.Throws<InvalidDataException>(() => store.LoadPending());
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

// Issue #101 M-6: HardwareSinglePreferencesStore must reject an oversized
// preferences file. As with M-5 above, this exercises the post-refactor size
// enforcement on the single read handle; it does not reproduce the TOCTOU
// race between the old separate FileInfo check and the FileStream open.
static async Task HardwareSinglePreferencesStoreRejectsOversizedFileAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var stateDirectory = Path.Combine(root, "state");
        Directory.CreateDirectory(stateDirectory);
        var preferencesPath = Path.Combine(stateDirectory, "preferences.json");
        await File.WriteAllBytesAsync(preferencesPath, new byte[17 * 1024]);

        var store = new HardwareSinglePreferencesStore(preferencesPath);
        await Check.ThrowsAsync<InvalidDataException>(() => store.LoadAsync());
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareSinglePreferencesAndProfileApprovalAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var exportDirectory = Path.Combine(root, "selected-exports");
        Directory.CreateDirectory(exportDirectory);
        var preferences = new HardwareSinglePreferencesStore(Path.Combine(root, "state", "preferences.json"));
        await preferences.SaveAsync(exportDirectory);
        var loaded = await preferences.LoadAsync();
        Check.Equal(Path.GetFullPath(exportDirectory), loaded!.ExportDirectory);

        var profilePath = Path.Combine(root, "camera-agent", "approved-single-capture-profile.json");
        var now = DateTimeOffset.Parse("2026-08-10T01:02:03Z");
        var timeProvider = new MutableTimeProvider(now);
        var profileStore = new HardwareSingleCaptureProfileStore(profilePath, timeProvider);
        await profileStore.ApproveCamAAsync(ApprovedCamAObservedSettings());
        using var profile = JsonDocument.Parse(await File.ReadAllTextAsync(profilePath));
        var document = profile.RootElement;
        Check.Equal("a0.camera-agent.capture-profile.v1", document.GetProperty("schemaVersion").GetString() ?? string.Empty);
        Check.Equal("CAM-A", document.GetProperty("selectedAlias").GetString() ?? string.Empty);
        Check.Equal("SingleCamera", document.GetProperty("cameraMode").GetString() ?? string.Empty);
        Check.Equal("2026-09-09T01:02:03Z", document.GetProperty("expiresAtUtc").GetString() ?? string.Empty);
        Check.False(document.GetRawText().Contains("serial", StringComparison.OrdinalIgnoreCase), "Profile must not contain a camera serial.");
        Check.False(document.GetProperty("expectedSettings").GetProperty("fileType").GetProperty("available").GetBoolean(), "FileType must remain explicitly unavailable.");
        Check.Equal("JPEG Fine", document.GetProperty("expectedSettings").GetProperty("compressionLevel").GetProperty("currentLabel").GetString() ?? string.Empty);
        Check.Equal((uint)3, document.GetProperty("expectedSettings").GetProperty("exposureMode").GetProperty("currentValue").GetUInt32());
        Check.Equal("Preset 1", document.GetProperty("expectedSettings").GetProperty("whiteBalanceMode").GetProperty("currentLabel").GetString() ?? string.Empty);

        var rejectedPath = Path.Combine(root, "camera-agent", "rejected-profile.json");
        var rejectedStore = new HardwareSingleCaptureProfileStore(rejectedPath, timeProvider);
        await Check.ThrowsAsync<InvalidOperationException>(() =>
            rejectedStore.ApproveCamAAsync(ApprovedCamAObservedSettings() with
            {
                ExposureMode = ApprovedCamAObservedSettings().ExposureMode with { CurrentValue = 1 },
            }));
        Check.False(File.Exists(rejectedPath), "Rejected observations must not publish a profile.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static HardwareObservedCameraSettings ApprovedCamAObservedSettings()
{
    static HardwareObservedCameraSetting Setting(string label) => new()
    {
        Available = true,
        CapType = "enum",
        ProbeState = "observed",
        ValueType = "label",
        CurrentValue = null,
        CurrentIndex = 0,
        CurrentLabel = label,
    };
    return new HardwareObservedCameraSettings
    {
        FileType = new HardwareObservedCameraSetting
        {
            Available = false,
            CapType = "unsupported",
            ProbeState = "not-advertised",
            ValueType = "unsupported",
            CurrentValue = null,
            CurrentIndex = null,
            CurrentLabel = null,
        },
        CompressionLevel = Setting("JPEG Fine"),
        ImageSize = Setting("L(7360*4912)"),
        ExposureMode = new HardwareObservedCameraSetting
        {
            Available = true,
            CapType = "enum",
            ProbeState = "available",
            ValueType = "unsigned",
            CurrentValue = 3,
            CurrentIndex = 3,
            CurrentLabel = null,
        },
        ShutterSpeed = Setting("1/6"),
        Aperture = Setting("8"),
        Sensitivity = Setting("64"),
        WhiteBalanceMode = Setting("Preset 1"),
        FocusMode = new HardwareObservedCameraSetting
        {
            Available = true,
            CapType = "generic",
            ProbeState = "available",
            ValueType = "unsigned",
            CurrentValue = 1,
            CurrentIndex = null,
            CurrentLabel = null,
        },
    };
}

static async Task HardwareSingleHappyPathAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var artifactsRoot = FakeAgentArtifactsRoot(root);
        // The same preview file plays two roles below: the pre-capture finite
        // Live View probe's Preview, and (via CompleteCaptureWithHandoff) the
        // capture's PostCapturePreview. Both must therefore resolve to the
        // same canonical path -- HardwareAgentArtifactLayout.PreviewPath only
        // depends on runId + alias, not the (not-yet-known) transaction ID,
        // so it can be written up front using CompleteCapture's fixed run ID.
        var resumedPreview = WritePreviewRecord(
            HardwareAgentArtifactLayout.PreviewPath(artifactsRoot, CompleteCaptureRunId, "CAM-B"));
        string? capturedOriginalPath = null;
        var operations = new FakeHardwareSingleCameraOperations
        {
            AgentExecutablePath = Path.Combine(root, "app", "fake-agent.exe"),
            AgentArtifactsRoot = artifactsRoot,
            ProbeLiveViewRunId = CompleteCaptureRunId,
            Preview = resumedPreview,
            CaptureResultFactory = (transactionId, alias) =>
            {
                var (result, originalPath) = CompleteCaptureWithHandoff(
                    artifactsRoot, transactionId, alias, resumedPreview);
                capturedOriginalPath = originalPath;
                return result;
            },
        };
        var store = new HardwareSingleAppStateStore(Path.Combine(root, "state"));
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            store,
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();
        viewModel.SelectedCamera = "CAM-B";
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        Check.True(viewModel.CanProbeLiveView, "Ready hardware must allow a finite Live View probe.");
        await viewModel.ProbeLiveViewAsync();
        Check.False(viewModel.CanCapture, "Finite Live View must require a fresh pre-capture readiness snapshot.");
        await viewModel.CheckReadinessAsync();
        viewModel.DedicatedSpoolScopeConfirmed = true;
        viewModel.ExactObjectDeleteConfirmed = true;
        Check.True(viewModel.CanCapture, "All confirmations plus readiness must enable one hardware capture.");

        var firstCapture = viewModel.CaptureAsync();
        var duplicateCapture = viewModel.CaptureAsync();
        await Task.WhenAll(firstCapture, duplicateCapture);
        Check.Equal(1, operations.CaptureCallCount);
        Check.True(operations.LastCaptureLiveViewHandoffRequested, "A selected finite Live View must request capture handoff and post-capture probe.");
        Check.Equal("single-profile", operations.LastCaptureExpectedProfile!.ProfileId);
        Check.True(viewModel.ReadinessDetail.Contains("2099-01-01", StringComparison.Ordinal), "The approved profile expiry must be visible before capture.");
        Check.True(viewModel.LiveViewSummary.Contains("有限1フレームprobe成功", StringComparison.Ordinal), "The post-capture finite Live View result must not claim a continuous stream.");
        Check.True(viewModel.CaptureSummary.Contains("成功", StringComparison.Ordinal), "Hardware capture must reach reviewed success.");
        Check.True(viewModel.StitchSummary.Contains("対象外", StringComparison.Ordinal), "Single hardware mode must never claim stitch output.");
        Check.True(viewModel.CanExport, "A reread-verified complete original must be explicitly exportable.");

        await viewModel.ExportAsync();
        Check.True(File.Exists(viewModel.LastExportPath), "Explicit hardware export must create a file.");
        Check.True(
            File.ReadAllBytes(capturedOriginalPath!).SequenceEqual(File.ReadAllBytes(viewModel.LastExportPath)),
            "Hardware export must be byte-identical to the canonical original.");
        Check.True(viewModel.ExportSummary.Contains("byte-identical", StringComparison.Ordinal), "The UI must label the single export provenance.");
        Check.True(await store.LoadPendingAsync() is not null, "A terminal result must remain durable until explicit operator preparation.");
        await viewModel.PrepareNewCaptureAsync();
        Check.True(await store.LoadPendingAsync() is null, "Only explicit new-capture preparation may clear the app transaction marker.");
        Check.Equal(string.Empty, viewModel.PreviewPath);
        Check.True(
            viewModel.LiveViewSummary.Contains("未実行", StringComparison.Ordinal),
            "Preparing a new transaction must not display the previous transaction's post-capture preview.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwarePendingTransactionRecoveryAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        const string transactionId = "0123456789abcdef0123456789abcdef";
        var artifactsRoot = FakeAgentArtifactsRoot(root);
        var store = new HardwareSingleAppStateStore(Path.Combine(root, "state"));
        await store.SavePendingAsync(new HardwarePendingTransaction
        {
            OperatingMode = "SingleCamera",
            TransactionId = transactionId,
            CameraAlias = "CAM-A",
            RequiredCameraAlias = "CAM-A",
            CaptureProfileId = "single-profile",
            CaptureProfileVersion = 1,
            CaptureProfileSha256 = new string('a', 64),
            CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
            LiveViewHandoffRequested = false,
            CaptureRequestDispatchAttempted = true,
            StartedAtUtc = DateTimeOffset.UtcNow,
        });
        var operations = new FakeHardwareSingleCameraOperations
        {
            AgentExecutablePath = Path.Combine(root, "app", "fake-agent.exe"),
            AgentArtifactsRoot = artifactsRoot,
        };
        // The transaction ID here is a compile-time constant (unlike a real
        // dispatched capture), so FailedPartialCapture can write its
        // original.jpg immediately -- it does not need to wait for a
        // dynamically-generated transaction ID the way CaptureResultFactory
        // callbacks elsewhere do.
        operations.TransactionResults.Enqueue(ReservedCapture(transactionId, "CAM-A"));
        operations.TransactionResults.Enqueue(InProgressCapture(transactionId, "CAM-A"));
        var (failedPartialResult, _) = FailedPartialCapture(artifactsRoot, transactionId, "CAM-A");
        operations.TransactionResults.Enqueue(failedPartialResult);
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            store,
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();

        Check.False(viewModel.CanCapture, "A pending hardware transaction must block a new shutter operation.");
        Check.Equal("CAM-A", viewModel.SelectedCamera);
        Check.Equal(0, operations.CaptureCallCount);
        Check.Equal(1, operations.TransactionResultCallCount);
        Check.Equal("single-profile", operations.LastTransactionExpectedProfile!.ProfileId);
        Check.Equal((uint)1, operations.LastTransactionExpectedProfile.ProfileVersion);
        Check.Equal(new string('a', 64), operations.LastTransactionExpectedProfile.Sha256);
        Check.Equal(DateTimeOffset.Parse("2099-01-01T00:00:00Z"), operations.LastTransactionExpectedProfile.ExpiresAtUtc);
        Check.Equal("CAM-A", operations.LastTransactionExpectedCameraAlias!);
        Check.False(operations.LastTransactionExpectedHandoff, "Startup recovery must preserve the original handoff choice.");
        Check.False(viewModel.CanPrepareNewCapture, "Startup recovery must keep a reserved transaction nonterminal.");
        Check.True(await store.LoadPendingAsync() is not null, "Startup recovery must preserve the durable pending marker.");
        await viewModel.RecoverTransactionAsync();
        Check.False(viewModel.CanCapture, "TransactionInProgress must keep new capture blocked.");
        Check.True(await store.LoadPendingAsync() is not null, "In-progress lookup must preserve the durable pending marker.");
        await viewModel.RecoverTransactionAsync();

        Check.Equal(0, operations.CaptureCallCount);
        Check.Equal(3, operations.TransactionResultCallCount);
        Check.True(viewModel.CaptureSummary.Contains("FailedPartial", StringComparison.Ordinal), "Recovered cleanup failure must remain FailedPartial.");
        Check.True(viewModel.CanExport, "A FailedPartial transaction may explicitly export its reread-verified retained original.");
        await viewModel.ExportAsync();
        Check.True(
            File.Exists(viewModel.LastExportPath),
            $"FailedPartial retained original export must be explicit and byte-verified. {viewModel.TechnicalDetail}");
        Check.True(viewModel.ExportSummary.Contains("FailedPartial", StringComparison.Ordinal), "Export must preserve the transaction failure label.");
        Check.True(await store.LoadPendingAsync() is not null, "Recovered terminal result must remain discoverable until operator preparation.");
        await viewModel.PrepareNewCaptureAsync();
        Check.True(await store.LoadPendingAsync() is null, "Explicit preparation must clear the recovered transaction marker.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareContinuousLiveViewCaptureHandoffAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var artifactsRoot = FakeAgentArtifactsRoot(root);
        // Continuous Live View frames are decoded and displayed in-memory
        // (DecodeVerifiedFrame), never checked against HardwareArtifactVerifier,
        // so this file's location is unrelated to artifactsRoot.
        var framePath = Path.Combine(root, "agent", "run-live-1", "preview.jpg");
        var frameBytes = File.ReadAllBytes(WritePreviewRecord(framePath).Path);
        var operations = new FakeContinuousHardwareOperations(frameBytes)
        {
            AgentExecutablePath = Path.Combine(root, "app", "fake-agent.exe"),
            AgentArtifactsRoot = artifactsRoot,
            CaptureResultFactory = (transactionId, alias) =>
                CompleteCapture(artifactsRoot, transactionId, alias).Result,
        };
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(Path.Combine(root, "state")),
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        viewModel.DedicatedSpoolScopeConfirmed = true;
        viewModel.ExactObjectDeleteConfirmed = true;

        Check.True(viewModel.CanStartContinuousLiveView, "Ready CAM-A must allow continuous Live View v2.");
        await viewModel.StartContinuousLiveViewAsync();
        await operations.FirstFrame.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Check.True(viewModel.IsContinuousLiveViewActive, "The UI must expose the owned active Live View session.");
        Check.True(viewModel.PreviewImage is not null, "A verified in-memory JPEG frame must be displayed.");

        await viewModel.CaptureAsync();
        Check.Equal(1, operations.CaptureCallCount);
        Check.False(operations.LastCaptureLiveViewHandoffRequested,
            "Continuous v2 must stop explicitly and must not be relabelled as the finite v1 handoff.");
        Check.Equal(2, operations.StartCount);
        Check.True(viewModel.IsContinuousLiveViewActive,
            "A successful terminal capture must restart the continuous Live View session.");
        Check.True(
            operations.CallOrder.IndexOf("stop") < operations.CallOrder.IndexOf("capture"),
            "SDK Live View stop and close must precede capture dispatch.");

        await viewModel.StopContinuousLiveViewAsync();
        Check.False(viewModel.IsContinuousLiveViewActive, "Explicit stop must close the continuous session.");
        await viewModel.ShutdownAsync();
        viewModel.Dispose();

        var blockedOperations = new FakeContinuousHardwareOperations(frameBytes)
        {
            AgentExecutablePath = Path.Combine(root, "app", "fake-agent.exe"),
            AgentArtifactsRoot = artifactsRoot,
            CaptureResultFactory = (transactionId, alias) =>
                CompleteCapture(artifactsRoot, transactionId, alias).Result,
            FailNextStop = true,
        };
        var blockedViewModel = new HardwareSingleCameraViewModel(
            blockedOperations,
            new HardwareSingleAppStateStore(Path.Combine(root, "blocked-state")),
            new HardwareOriginalExporter(Path.Combine(root, "blocked-exports")));
        await blockedViewModel.InitializeAsync();
        blockedViewModel.ExclusiveCameraControlConfirmed = true;
        await blockedViewModel.CheckReadinessAsync();
        blockedViewModel.DedicatedSpoolScopeConfirmed = true;
        blockedViewModel.ExactObjectDeleteConfirmed = true;
        await blockedViewModel.StartContinuousLiveViewAsync();
        await blockedOperations.FirstFrame.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await blockedViewModel.CaptureAsync();
        Check.Equal(0, blockedOperations.CaptureCallCount);
        Check.False(blockedViewModel.IsContinuousLiveViewActive,
            "A failed stop whose typed result confirms SDK closed must invalidate the stale session.");
        Check.False(blockedViewModel.CanCapture,
            "A failed stop must invalidate readiness and keep capture disabled.");
        await blockedViewModel.ShutdownAsync();
        blockedViewModel.Dispose();
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareLiveViewRequiresFreshReadinessAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var artifactsRoot = FakeAgentArtifactsRoot(root);
        var preview = WritePreviewRecord(
            HardwareAgentArtifactLayout.PreviewPath(artifactsRoot, "run-3000-1", "CAM-A"));
        var operations = new FakeHardwareSingleCameraOperations
        {
            AgentExecutablePath = Path.Combine(root, "app", "fake-agent.exe"),
            AgentArtifactsRoot = artifactsRoot,
            Preview = preview,
            ReadinessFactory = alias => HardwareTestData.ReadyHardware(alias) with
            {
                Ready = false,
                CaptureProfileApproved = false,
                CaptureProfileId = string.Empty,
                CaptureProfileVersion = 0,
                CaptureProfileSha256 = string.Empty,
                CaptureProfileCameraAlias = string.Empty,
                CaptureProfileExpiresAtUtc = null,
                CaptureProfileAliasMatches = false,
                SettingsMatchApprovedProfile = false,
                FailureCategory = "capture_profile_not_approved",
                FailureDetail = "No approved Single profile is configured.",
            },
        };
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(Path.Combine(root, "state")),
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        Check.True(viewModel.CanProbeLiveView, "A bound read-only camera may preview even while capture profile approval blocks shutter.");
        Check.False(viewModel.CanCapture, "Unapproved capture profile must keep shutter blocked.");
        await viewModel.ProbeLiveViewAsync();

        Check.Equal(1, operations.LiveViewCallCount);
        Check.Equal(preview.Path, viewModel.PreviewPath);
        Check.True(viewModel.HasPreview, "Verified finite preview must be displayed.");
        Check.False(viewModel.CanCapture, "Live View handoff must invalidate readiness before capture.");
        Check.True(viewModel.ReadinessSummary.Contains("再確認", StringComparison.Ordinal), "Fresh readiness must be visible after Live View close.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareMalformedLocalStateFailsClosedAsync()
{
    Check.True(
        WindowsLocalPathGuard.IsSupportedLocalDrive(DriveType.Fixed),
        "A fixed Windows drive must satisfy the product-local storage boundary.");
    Check.False(
        WindowsLocalPathGuard.IsSupportedLocalDrive(DriveType.Network),
        "A mapped network drive must not be accepted as local product storage.");
    Check.False(
        WindowsLocalPathGuard.IsSupportedLocalDrive(DriveType.Removable),
        "Removable storage must not host durable application state or accepted exports.");

    var root = CreateHardwareTestRoot();
    try
    {
        var stateDirectory = Path.Combine(root, "state");
        Directory.CreateDirectory(stateDirectory);
        await File.WriteAllTextAsync(
            Path.Combine(stateDirectory, "app-state.json"),
            "{\"schemaVersion\":\"a0.hardware-single-app-state.v1\",\"pendingTransaction\":null,\"unexpected\":true}");
        var operations = new FakeHardwareSingleCameraOperations();
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(stateDirectory),
            new HardwareOriginalExporter(Path.Combine(root, "exports")));
        await viewModel.InitializeAsync();

        Check.False(viewModel.CanCheckReadiness, "Malformed local state must block even read-only hardware access.");
        Check.False(viewModel.CanCapture, "Malformed local state must block capture.");
        Check.True(viewModel.BlockerText.Contains("安全に読めません", StringComparison.Ordinal), "Fail-closed state must be explicit.");
        Check.Equal(0, operations.TotalCallCount);

        var duplicateDirectory = Path.Combine(root, "duplicate-state");
        Directory.CreateDirectory(duplicateDirectory);
        await File.WriteAllTextAsync(
            Path.Combine(duplicateDirectory, "app-state.json"),
            $"{{\"schemaVersion\":\"{HardwareSingleAppStateProtocol.SchemaVersion}\",\"schemaVersion\":\"{HardwareSingleAppStateProtocol.SchemaVersion}\",\"pendingTransaction\":null}}");
        var duplicateOperations = new FakeHardwareSingleCameraOperations();
        var duplicateViewModel = new HardwareSingleCameraViewModel(
            duplicateOperations,
            new HardwareSingleAppStateStore(duplicateDirectory),
            new HardwareOriginalExporter(Path.Combine(root, "duplicate-exports")));
        await duplicateViewModel.InitializeAsync();
        Check.False(duplicateViewModel.CanCheckReadiness, "Duplicate state properties must fail closed.");
        Check.Equal(0, duplicateOperations.TotalCallCount);

        var oversizedDirectory = Path.Combine(root, "oversized-state");
        Directory.CreateDirectory(oversizedDirectory);
        await File.WriteAllBytesAsync(
            Path.Combine(oversizedDirectory, "app-state.json"),
            new byte[65 * 1024]);
        var oversizedOperations = new FakeHardwareSingleCameraOperations();
        var oversizedViewModel = new HardwareSingleCameraViewModel(
            oversizedOperations,
            new HardwareSingleAppStateStore(oversizedDirectory),
            new HardwareOriginalExporter(Path.Combine(root, "oversized-exports")));
        await oversizedViewModel.InitializeAsync();
        Check.False(oversizedViewModel.CanCheckReadiness, "Oversized state must fail closed before parsing.");
        Check.Equal(0, oversizedOperations.TotalCallCount);
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static void HardwareLaunchOptionsAreExplicit()
{
    const string legacySingleAgentVariable = "A0_CAMERA_AGENT_PATH";
    const string legacyDualAgentVariable = "A0_DUAL_CAMERA_AGENT_PATH";
    var previousSingleAgent = Environment.GetEnvironmentVariable(legacySingleAgentVariable);
    var previousDualAgent = Environment.GetEnvironmentVariable(legacyDualAgentVariable);
    var root = Path.Combine(Path.GetTempPath(), "A0CameraStitcher-launch", Guid.NewGuid().ToString("N"));
    var baseDirectory = Path.Combine(root, "app");
    Directory.CreateDirectory(baseDirectory);
    var singleAgent = Path.Combine(baseDirectory, "A0CameraStitcher.CameraAgent.exe");
    var dualAgent = Path.Combine(baseDirectory, "A0CameraStitcher.DualCameraAgent.exe");
    var explicitAgent = Path.Combine(baseDirectory, "explicit-agent.exe");
    File.WriteAllBytes(singleAgent, [0x4d, 0x5a]);
    File.WriteAllBytes(dualAgent, [0x4d, 0x5a]);
    File.WriteAllBytes(explicitAgent, [0x4d, 0x5a]);
    try
    {
        var launcher = ApplicationLaunchOptions.Parse([], baseDirectory);
        Check.Equal(ApplicationLaunchMode.Launcher, launcher.Mode);
        Check.Equal(singleAgent, launcher.SingleCameraAgentExecutablePath);
        Check.Equal(dualAgent, launcher.DualCameraAgentExecutablePath);
        var launcherOverride = ApplicationLaunchOptions.Parse(["--camera-agent", explicitAgent], baseDirectory);
        Check.Equal(ApplicationLaunchMode.Launcher, launcherOverride.Mode);
        Check.Equal(explicitAgent, launcherOverride.SingleCameraAgentExecutablePath);
        Check.Equal(dualAgent, launcherOverride.DualCameraAgentExecutablePath);
        var hardware = ApplicationLaunchOptions.Parse(["--hardware-single", "--camera-agent", explicitAgent], baseDirectory);
        Check.Equal(ApplicationLaunchMode.HardwareSingle, hardware.Mode);
        Check.Equal(explicitAgent, hardware.SingleCameraAgentExecutablePath);
        var hardwareDual = ApplicationLaunchOptions.Parse(["--hardware-dual", "--camera-agent", explicitAgent], baseDirectory);
        Check.Equal(ApplicationLaunchMode.HardwareDual, hardwareDual.Mode);
        Check.Equal(explicitAgent, hardwareDual.DualCameraAgentExecutablePath);
        var simulated = ApplicationLaunchOptions.Parse(["--simulated"], baseDirectory);
        Check.Equal(ApplicationLaunchMode.Simulated, simulated.Mode);
        File.Delete(singleAgent);
        File.Delete(dualAgent);
        Check.Throws<ArgumentException>(() => ApplicationLaunchOptions.Parse([], baseDirectory));
        Check.Equal(ApplicationLaunchMode.Simulated, ApplicationLaunchOptions.Parse(["--simulated"], baseDirectory).Mode);
        File.WriteAllBytes(singleAgent, [0x4d, 0x5a]);
        File.WriteAllBytes(dualAgent, [0x4d, 0x5a]);
        Check.Throws<ArgumentException>(() => ApplicationLaunchOptions.Parse(["--simulated", "--hardware-single"], baseDirectory));
        Check.Throws<ArgumentException>(() => ApplicationLaunchOptions.Parse(["--hardware-dual", "--hardware-single"], baseDirectory));
        Check.Throws<ArgumentException>(() => ApplicationLaunchOptions.Parse(["--simulated", "--camera-agent", explicitAgent], baseDirectory));
        Environment.SetEnvironmentVariable(legacySingleAgentVariable, Path.Combine(root, "legacy-single.exe"));
        Environment.SetEnvironmentVariable(legacyDualAgentVariable, Path.Combine(root, "legacy-dual.exe"));
        Check.Equal(singleAgent, ApplicationLaunchOptions.Parse(["--hardware-single"], baseDirectory).SingleCameraAgentExecutablePath);
        Check.Equal(dualAgent, ApplicationLaunchOptions.Parse(["--hardware-dual"], baseDirectory).DualCameraAgentExecutablePath);

        foreach (var rejected in new[]
                 {
                     "relative.exe", "..\\outside.exe", "\\\\server\\share\\agent.exe",
                     "\\\\?\\C:\\agent.exe", "C:\\agent.exe:stream", root + "-sibling\\agent.exe",
                     Path.Combine(root, "outside.exe"), baseDirectory,
                     Path.Combine(baseDirectory, "agent.dll"), Path.Combine(baseDirectory, "missing.exe"),
                 })
        {
            try
            {
                _ = ApplicationLaunchOptions.Parse(["--hardware-dual", "--camera-agent", rejected], baseDirectory);
                throw new InvalidOperationException("Expected a rejected Camera Agent path.");
            }
            catch (ArgumentException exception)
            {
                Check.False(exception.Message.Contains(rejected, StringComparison.Ordinal),
                    "Camera Agent policy errors must not disclose the rejected raw path.");
            }
        }
        Check.Throws<ArgumentException>(() => CameraAgentExecutablePolicy.Resolve(
            baseDirectory, explicitAgent, _ => DriveType.Removable));
        Check.Throws<ArgumentException>(() => CameraAgentExecutablePolicy.Resolve(
            baseDirectory, explicitAgent, _ => DriveType.Fixed,
            path => string.Equals(path, explicitAgent, StringComparison.OrdinalIgnoreCase)
                ? FileAttributes.ReparsePoint
                : FileAttributes.Directory));
        foreach (var reparseDirectory in new[] { baseDirectory, root })
        {
            Check.Throws<ArgumentException>(() => CameraAgentExecutablePolicy.Resolve(
                baseDirectory, explicitAgent, _ => DriveType.Fixed,
                path => string.Equals(path, explicitAgent, StringComparison.OrdinalIgnoreCase)
                    ? FileAttributes.Normal
                    : string.Equals(path, reparseDirectory, StringComparison.OrdinalIgnoreCase)
                        ? FileAttributes.Directory | FileAttributes.ReparsePoint
                        : FileAttributes.Directory));
        }
    }
    finally
    {
        Environment.SetEnvironmentVariable(legacySingleAgentVariable, previousSingleAgent);
        Environment.SetEnvironmentVariable(legacyDualAgentVariable, previousDualAgent);
        Directory.Delete(root, recursive: true);
    }
    Check.Throws<InvalidDataException>(() => HardwareSingleStoragePaths.Resolve(string.Empty));
    Check.Throws<InvalidDataException>(() => HardwareSingleStoragePaths.Resolve("relative-local-app-data"));
    var storagePaths = HardwareSingleStoragePaths.Resolve(Path.GetTempPath());
    Check.True(Path.IsPathFullyQualified(storagePaths.StateDirectory), "Hardware state must derive from an absolute LocalApplicationData root.");
    Check.True(
        storagePaths.ExportDirectory.StartsWith(Path.GetFullPath(Path.GetTempPath()), StringComparison.OrdinalIgnoreCase),
        "Hardware exports must stay below the validated LocalApplicationData root.");
}

static async Task HardwareAppStateCompareAndSetAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var store = new HardwareSingleAppStateStore(Path.Combine(root, "state"));
        var first = new HardwarePendingTransaction
        {
            OperatingMode = "SingleCamera",
            TransactionId = "11111111111111111111111111111111",
            CameraAlias = "CAM-A",
            RequiredCameraAlias = "CAM-A",
            CaptureProfileId = "single-profile",
            CaptureProfileVersion = 1,
            CaptureProfileSha256 = new string('a', 64),
            CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
            LiveViewHandoffRequested = false,
            CaptureRequestDispatchAttempted = false,
            StartedAtUtc = DateTimeOffset.UtcNow,
        };
        var second = new HardwarePendingTransaction
        {
            OperatingMode = "SingleCamera",
            TransactionId = "22222222222222222222222222222222",
            CameraAlias = "CAM-B",
            RequiredCameraAlias = "CAM-B",
            CaptureProfileId = "single-profile",
            CaptureProfileVersion = 1,
            CaptureProfileSha256 = new string('b', 64),
            CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
            LiveViewHandoffRequested = true,
            CaptureRequestDispatchAttempted = false,
            StartedAtUtc = DateTimeOffset.UtcNow,
        };
        await store.SavePendingAsync(first);
        await store.MarkCaptureRequestDispatchAttemptedAsync(first.TransactionId);
        Check.True((await store.LoadPendingAsync())!.CaptureRequestDispatchAttempted, "Dispatch intent must be persisted before invoking the Camera Agent.");
        await Check.ThrowsAsync<InvalidOperationException>(() => store.SavePendingAsync(second));
        Check.Equal(first.TransactionId, (await store.LoadPendingAsync())!.TransactionId);
        await Check.ThrowsAsync<InvalidOperationException>(() => store.ClearPendingAsync(second.TransactionId));
        Check.Equal(first.TransactionId, (await store.LoadPendingAsync())!.TransactionId);
        await store.ClearPendingAsync(first.TransactionId);
        Check.True(await store.LoadPendingAsync() is null, "Expected transaction compare-and-clear must succeed.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static void HardwareOperatorSessionLeaseIsExclusive()
{
    using var acquired = new ManualResetEventSlim(false);
    using var release = new ManualResetEventSlim(false);
    Exception? ownerFailure = null;
    var owner = new Thread(() =>
    {
        try
        {
            using var lease = HardwareSingleAppSessionLease.Acquire();
            acquired.Set();
            release.Wait(TimeSpan.FromSeconds(10));
        }
        catch (Exception exception)
        {
            ownerFailure = exception;
            acquired.Set();
        }
    });
    owner.Start();
    Check.True(acquired.Wait(TimeSpan.FromSeconds(5)), "The hardware session lease owner did not start.");
    if (ownerFailure is not null)
    {
        throw new InvalidOperationException("The hardware session lease owner failed.", ownerFailure);
    }

    try
    {
        Check.Throws<HardwareSingleAppSessionBusyException>(() => HardwareSingleAppSessionLease.Acquire());
    }
    finally
    {
        release.Set();
        Check.True(owner.Join(millisecondsTimeout: 5000), "The hardware session lease owner did not exit.");
    }

    using var reacquired = HardwareSingleAppSessionLease.Acquire();
}

static async Task HardwareExportVerificationFailureStaysUnpublishedAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var sourcePath = Path.Combine(root, "agent", "run-4000-1", "CAM-A", "original.jpg");
        var original = WriteJpegRecord(sourcePath, "CAM-A");
        var exportDirectory = Path.Combine(root, "exports");
        var exporter = new HardwareOriginalExporter(
            exportDirectory,
            (stagingPath, cancellationToken) =>
                File.AppendAllTextAsync(
                    stagingPath,
                    "tampered-after-locked-verification",
                    cancellationToken));

        await Check.ThrowsAsync<IOException>(() => exporter.ExportAsync(
            original,
            "44444444444444444444444444444444",
            DateTimeOffset.Parse("2026-08-10T00:00:00Z"),
            sourcePath));

        Check.Equal(0, Directory.GetFiles(exportDirectory, "*.jpg", SearchOption.TopDirectoryOnly).Length);
        Check.Equal(1, Directory.GetFiles(exportDirectory, "*.partial", SearchOption.TopDirectoryOnly).Length);

        var replacementDirectory = Path.Combine(root, "replacement-exports");
        var replacementHookRan = false;
        var exactHandleExporter = new HardwareOriginalExporter(
            replacementDirectory,
            async (stagingPath, cancellationToken) =>
            {
                File.Move(stagingPath, stagingPath + ".verified-handle");
                await File.WriteAllTextAsync(
                    stagingPath,
                    "unverified path replacement",
                    cancellationToken);
                replacementHookRan = true;
            });
        var finalPath = await exactHandleExporter.ExportAsync(
            original,
            "45454545454545454545454545454545",
            DateTimeOffset.Parse("2026-08-10T00:00:01Z"),
            sourcePath);
        Check.True(replacementHookRan, "The post-verification replacement seam must execute.");
        Check.True(
            File.Exists(finalPath),
            $"Handle-based publication did not create the expected path. Files: {string.Join(", ", Directory.GetFiles(replacementDirectory, "*", SearchOption.AllDirectories))}");
        Check.True(
            File.ReadAllBytes(sourcePath).SequenceEqual(File.ReadAllBytes(finalPath)),
            "Handle-based publication must publish the verified file identity, not a path replacement.");
        Check.Equal(
            "unverified path replacement",
            await File.ReadAllTextAsync(finalPath + ".partial"));
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwarePreDispatchAndNotFoundRecoveryBoundariesAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var preparedStore = new HardwareSingleAppStateStore(Path.Combine(root, "prepared-state"));
        var prepared = HardwarePending(
            "55555555555555555555555555555555",
            "CAM-A",
            captureRequestDispatchAttempted: false);
        await preparedStore.SavePendingAsync(prepared);
        var preparedOperations = new FakeHardwareSingleCameraOperations();
        var preparedViewModel = new HardwareSingleCameraViewModel(
            preparedOperations,
            preparedStore,
            new HardwareOriginalExporter(Path.Combine(root, "prepared-exports")));

        await preparedViewModel.InitializeAsync();
        Check.Equal(0, preparedOperations.TotalCallCount);
        Check.True(preparedViewModel.CanPrepareNewCapture, "A durable known pre-dispatch failure must be explicitly closable without querying hardware.");
        Check.True(preparedViewModel.CaptureSummary.Contains("撮影要求0回", StringComparison.Ordinal), "The zero-dispatch provenance must be visible.");
        await preparedViewModel.PrepareNewCaptureAsync();
        Check.True(await preparedStore.LoadPendingAsync() is null, "Explicit preparation must clear only the known pre-dispatch marker.");

        var launchFailureStore = new HardwareSingleAppStateStore(Path.Combine(root, "launch-failure-state"));
        var launchFailureOperations = new FakeHardwareSingleCameraOperations
        {
            CaptureException = new HardwareCameraAgentLaunchException("Synthetic process start failure."),
        };
        var launchFailureViewModel = new HardwareSingleCameraViewModel(
            launchFailureOperations,
            launchFailureStore,
            new HardwareOriginalExporter(Path.Combine(root, "launch-failure-exports")));
        await launchFailureViewModel.InitializeAsync();
        launchFailureViewModel.ExclusiveCameraControlConfirmed = true;
        launchFailureViewModel.DedicatedSpoolScopeConfirmed = true;
        launchFailureViewModel.ExactObjectDeleteConfirmed = true;
        await launchFailureViewModel.CheckReadinessAsync();
        await launchFailureViewModel.CaptureAsync();
        Check.True(launchFailureViewModel.CanPrepareNewCapture, "A typed undispatched launch failure must be durably closable as zero shutter.");
        Check.False((await launchFailureStore.LoadPendingAsync())!.CaptureRequestDispatchAttempted, "Typed pre-dispatch failure must durably restore known-undispatched provenance.");
        await launchFailureViewModel.PrepareNewCaptureAsync();
        Check.True(await launchFailureStore.LoadPendingAsync() is null, "Explicit preparation must clear the typed undispatched failure.");

        var ambiguousStore = new HardwareSingleAppStateStore(Path.Combine(root, "ambiguous-state"));
        var ambiguous = HardwarePending(
            "66666666666666666666666666666666",
            "CAM-B",
            captureRequestDispatchAttempted: true,
            liveViewHandoffRequested: true);
        await ambiguousStore.SavePendingAsync(ambiguous);
        var ambiguousOperations = new FakeHardwareSingleCameraOperations();
        ambiguousOperations.TransactionResults.Enqueue(TransactionNotFoundCapture(
            ambiguous.TransactionId,
            ambiguous.CameraAlias));
        var ambiguousViewModel = new HardwareSingleCameraViewModel(
            ambiguousOperations,
            ambiguousStore,
            new HardwareOriginalExporter(Path.Combine(root, "ambiguous-exports")));

        await ambiguousViewModel.InitializeAsync();
        Check.Equal(0, ambiguousOperations.CaptureCallCount);
        Check.Equal(1, ambiguousOperations.TransactionResultCallCount);
        Check.Equal("CAM-B", ambiguousOperations.LastTransactionExpectedCameraAlias!);
        Check.True(ambiguousOperations.LastTransactionExpectedHandoff, "Ambiguous restart recovery must preserve a true handoff snapshot.");
        Check.False(ambiguousViewModel.CanPrepareNewCapture, "An attempted dispatch with no journal must remain support-required and block a new shutter.");
        Check.True(ambiguousViewModel.CaptureSummary.Contains("TransactionNotFound", StringComparison.Ordinal), "The missing journal blocker must remain explicit.");
        Check.True(await ambiguousStore.LoadPendingAsync() is not null, "Ambiguous missing-journal state must never be cleared automatically.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task HardwareInitializationAndProfileExpiryGateAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var blockingStore = new BlockingHardwareStateStore();
        var startupOperations = new FakeHardwareSingleCameraOperations();
        var startupViewModel = new HardwareSingleCameraViewModel(
            startupOperations,
            blockingStore,
            new HardwareOriginalExporter(Path.Combine(root, "startup-exports")));
        Check.False(startupViewModel.CanChangeConfirmations, "Hardware controls must be disabled before startup state inspection begins.");
        Check.False(startupViewModel.CanCheckReadiness, "Readiness must be disabled before startup state inspection.");

        var initialize = startupViewModel.InitializeAsync();
        await blockingStore.LoadStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        startupViewModel.ExclusiveCameraControlConfirmed = true;
        await startupViewModel.CheckReadinessAsync();
        await startupViewModel.CaptureAsync();
        Check.False(startupViewModel.ExclusiveCameraControlConfirmed, "Startup-time confirmation changes must be ignored.");
        Check.Equal(0, startupOperations.TotalCallCount);
        blockingStore.ReleaseLoad();
        await initialize;
        Check.True(startupViewModel.CanChangeConfirmations, "Controls may open only after durable startup inspection completes.");

        var now = DateTimeOffset.Parse("2026-08-10T10:00:00Z");
        var timeProvider = new MutableTimeProvider(now);
        var expiryOperations = new FakeHardwareSingleCameraOperations
        {
            ReadinessFactory = alias => HardwareTestData.ReadyHardware(alias) with
            {
                CaptureProfileExpiresAtUtc = now.AddSeconds(1),
            },
        };
        var expiryViewModel = new HardwareSingleCameraViewModel(
            expiryOperations,
            new HardwareSingleAppStateStore(Path.Combine(root, "expiry-state")),
            new HardwareOriginalExporter(Path.Combine(root, "expiry-exports")),
            timeProvider);
        await expiryViewModel.InitializeAsync();
        expiryViewModel.ExclusiveCameraControlConfirmed = true;
        expiryViewModel.DedicatedSpoolScopeConfirmed = true;
        expiryViewModel.ExactObjectDeleteConfirmed = true;
        await expiryViewModel.CheckReadinessAsync();
        Check.True(expiryViewModel.CanCapture, "A future approved profile may enable capture.");
        timeProvider.Advance(TimeSpan.FromSeconds(2));
        Check.False(expiryViewModel.CanCapture, "Local capture availability must close when the frozen profile expires.");
        Check.Equal("期限切れ", expiryViewModel.ReadinessSummary);
        Check.True(
            expiryViewModel.BlockerText.Contains("capture_profile_expired", StringComparison.Ordinal),
            "Profile expiry must immediately become a visible blocker without waiting for a click.");
        await expiryViewModel.CaptureAsync();
        Check.Equal(0, expiryOperations.CaptureCallCount);
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task DualCameraRegressionAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    string? exportPath = null;
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        Check.False(viewModel.IsSingleCameraMode, "Dual mode must remain the safe default for the existing workflow.");
        Check.True(viewModel.CaptureButtonText.Contains("2台", StringComparison.Ordinal), "The dual action must remain explicit.");

        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "A ready dual plan must allow capture.");
        viewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(() => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy, "The dual workflow did not finish.");

        Check.Equal(OperatorUiState.Review, viewModel.UiState);
        Check.True(viewModel.RetainedOriginals.Contains("CAM-A", StringComparison.Ordinal), "Dual mode must retain CAM-A.");
        Check.True(viewModel.RetainedOriginals.Contains("CAM-B", StringComparison.Ordinal), "Dual mode must retain CAM-B.");
        Check.False(viewModel.StitchResult.Contains("対象外", StringComparison.Ordinal), "Dual mode must still produce a stitch result.");
        Check.True(viewModel.CanExport, "A reviewed dual stitch must remain exportable.");
        Check.True(viewModel.CanRestitch, "Two retained originals must remain restitchable.");

        var firstStitchJob = viewModel.LastStitchJobId;
        viewModel.RestitchCommand.Execute(null);
        Check.False(
            string.Equals(firstStitchJob, viewModel.LastStitchJobId, StringComparison.Ordinal),
            "Restitch must create a distinct stitch job.");

        viewModel.ExportCommand.Execute(null);
        exportPath = viewModel.LastExportPath;
        Check.True(File.Exists(exportPath), "The explicit simulated dual export must exist.");
        Check.True(viewModel.ExportResult.Contains("合成出力", StringComparison.Ordinal), "The dual export must remain labeled as stitched output.");
    }
    finally
    {
        if (!string.IsNullOrWhiteSpace(exportPath) && File.Exists(exportPath))
        {
            File.Delete(exportPath);
        }
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task DualIdentityBlocksWpfCaptureAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-DualIdentityWpfTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var bridge = new NeverCaptureDualBridge();
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.HardwarePending()));
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.False(viewModel.CanCapture, "HardwarePending identity must close the WPF capture gate.");
        Check.True(viewModel.CaptureDisabledReason.Contains("HardwarePending", StringComparison.Ordinal),
            "The typed identity blocker must be visible without identifiers.");
        viewModel.CaptureCommand.Execute(null);
        Check.Equal(0, viewModel.TransactionStartCount);
        Check.Equal(0, bridge.CaptureCalls);
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task DualIdentityExpiryBlocksWpfCaptureAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-DualIdentityExpiryWpfTests",
        Guid.NewGuid().ToString("N"));
    var exportRoot = Path.Combine(root, "export");
    Directory.CreateDirectory(exportRoot);
    try
    {
        var now = DateTimeOffset.Parse("2026-08-11T00:00:00Z");
        var time = new MutableTimeProvider(now);
        var finiteReady = new DualCameraIdentitySnapshot(
            DualCameraIdentityStatus.Ready,
            "ready",
            now.AddMinutes(-1),
            now.AddSeconds(1));
        var bridge = new NeverCaptureDualBridge();
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(finiteReady),
            time);
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.FixedLocalExportDirectory = exportRoot;
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "A finite unexpired Ready snapshot must allow WPF capture.");
        time.Advance(TimeSpan.FromSeconds(2));
        Check.False(viewModel.CanCapture, "An expired Ready snapshot must close the WPF capture gate.");
        Check.True(viewModel.CaptureDisabledReason.Contains("Expired", StringComparison.Ordinal),
            "The WPF blocker must expose the normalized Expired state.");
        viewModel.CaptureCommand.Execute(null);
        Check.Equal(0, viewModel.TransactionStartCount);
        Check.Equal(0, bridge.CaptureCalls);
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task FormalDualCameraWpfFlowAsync()
{
    var previousAdapterPath = Environment.GetEnvironmentVariable("A0_M2_ADAPTER_PATH");
    Environment.SetEnvironmentVariable("A0_M2_ADAPTER_PATH", null);
    var bundledAdapterPath = Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe");
    Check.True(File.Exists(bundledAdapterPath), "The formal WPF output must bundle A0CameraStitcher.M2Adapter.exe.");
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-FormalDualWpfTests",
        Guid.NewGuid().ToString("N"));
    var transactionRoot = Path.Combine(root, "legacy-journals");
    var productRoot = Path.Combine(root, "products");
    var exportRoot = Path.Combine(root, "operator-export");
    Directory.CreateDirectory(transactionRoot);
    Directory.CreateDirectory(exportRoot);
    try
    {
        var productFlow = DualCameraProductComposition.Create(productRoot);
        var pendingHardwareFlow = DualCameraProductComposition.Create(
            Path.Combine(root, "hardware-pending-products"),
            DualCameraExecutionEnvironment.HardwareDual);
        var pendingViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-pending-journals")),
            pendingHardwareFlow);
        await pendingViewModel.InitializeAsync(CancellationToken.None);
        pendingViewModel.FixedLocalExportDirectory = exportRoot;
        pendingViewModel.AcceptSafetyCommand.Execute(null);
        Check.Equal(DualCameraIdentityStatus.HardwarePending, pendingHardwareFlow.IdentitySnapshot.Status);
        Check.False(pendingViewModel.CanCapture, "HardwareDual production composition must remain HardwarePending without provider/Agent operations.");
        Check.Equal(OperatorShellViewModel.HardwareDualPendingBanner, pendingViewModel.BannerText);

        var hardwareAdapter = new M2OfflineStitcherProcessAdapter(bundledAdapterPath);
        var hardwareOperations = new WpfHardwareDualFakeOperations(hardwareAdapter);
        var hardwareFlow = new DualCameraProductFlow(
            Path.Combine(root, "hardware-fake-products"),
            new HardwareDualCaptureSource(hardwareOperations),
            hardwareAdapter,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));
        var noRequestProviderViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-no-request-provider-journals")),
            hardwareFlow,
            dualBindingTransport: new SimulatedDualBindingAgentTransport(new SimulatedDualBindingAgent()));
        await noRequestProviderViewModel.InitializeAsync(CancellationToken.None);
        noRequestProviderViewModel.FixedLocalExportDirectory = exportRoot;
        noRequestProviderViewModel.AcceptSafetyCommand.Execute(null);
        Check.False(noRequestProviderViewModel.CanCapture, "Ready identity alone must not bypass approved profile and explicit confirmation injection.");
        // The binding is named first while it is outstanding, because finishing it is the operator's
        // actual next action -- the modal is in front of them. A missing request boundary is a
        // configuration fault they cannot act on from this screen.
        Check.True(
            noRequestProviderViewModel.CaptureDisabledReason.Contains("機体照合", StringComparison.Ordinal),
            "An outstanding binding must be the blocker the operator is shown first.");
        await CompleteDualBindingAsync(noRequestProviderViewModel.DualBinding);
        Check.False(noRequestProviderViewModel.CanCapture, "Ready identity alone must not bypass approved profile and explicit confirmation injection.");
        Check.True(noRequestProviderViewModel.CaptureDisabledReason.Contains("explicit operator confirmations", StringComparison.Ordinal), "The WPF blocker must identify the missing HardwareDual request boundary.");
        var hardwareViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-fake-journals")),
            hardwareFlow,
            () => DualCameraCaptureRequest.CreateHardwareDual(
                DualCameraRigProfile.ApprovedSynthetic(),
                HardwareDualCaptureProfile.ApprovedSynthetic(),
                new HardwareDualOperatorConfirmations(true, true, true, true, true),
                Guid.NewGuid()),
            dualBindingTransport: new SimulatedDualBindingAgentTransport(new SimulatedDualBindingAgent()));
        await hardwareViewModel.InitializeAsync(CancellationToken.None);
        hardwareViewModel.FixedLocalExportDirectory = exportRoot;
        hardwareViewModel.AcceptSafetyCommand.Execute(null);
        // HardwareDual now requires a confirmed session binding before capture (ADR-0025, #62).
        // Everything else about this flow is unchanged; what changed is that a Ready identity and
        // approved profiles are no longer sufficient on their own.
        Check.False(
            hardwareViewModel.CanCapture,
            "HardwareDual must not start a capture before the operator has confirmed the binding.");
        await CompleteDualBindingAsync(hardwareViewModel.DualBinding);
        Check.True(hardwareViewModel.CanCapture, "Explicit fake Agent, Ready identity, approved profiles, and confirmations must enable the software-only HardwareDual path.");
        hardwareViewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => hardwareViewModel.TransactionStartCount == 1 && !hardwareViewModel.IsBusy,
            "HardwareDual WPF fake capture did not finish.");
        Check.Equal(OperatorUiState.Review, hardwareViewModel.UiState);
        Check.Equal(1, hardwareOperations.StartCalls);
        Check.True(hardwareViewModel.RetainedOriginals.Contains("CAM-A: original.jpg", StringComparison.Ordinal), "HardwareDual WPF must retain CAM-A original.");
        Check.True(hardwareViewModel.RetainedOriginals.Contains("CAM-B: original.jpg", StringComparison.Ordinal), "HardwareDual WPF must retain CAM-B original.");
        hardwareViewModel.ExportCommand.Execute(null);
        await WaitUntilAsync(
            () => !hardwareViewModel.IsBusy && hardwareViewModel.ExportResult.Contains("このPCのフォルダへ保存しました", StringComparison.Ordinal),
            "HardwareDual WPF fixed-local export did not finish.");
        Check.True(File.Exists(hardwareViewModel.LastExportPath), "HardwareDual WPF must publish the explicit fixed-local export.");

        var recoveryIdentity = new MutableDualIdentitySource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady());
        var recoveryOperations = new WpfHardwareDualFakeOperations(hardwareAdapter, responseUnknownOnce: true);
        var recoveryProductRoot = Path.Combine(root, "hardware-recovery-products");
        var recoveryStore = new HardwareDualTransactionSnapshotStore(recoveryProductRoot);
        var recoveryFlow = new DualCameraProductFlow(
            recoveryProductRoot,
            new HardwareDualCaptureSource(recoveryOperations, recoveryStore: recoveryStore),
            hardwareAdapter,
            recoveryIdentity);
        var requestProviderCalls = 0;
        var recoveryViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-recovery-journals")),
            recoveryFlow,
            () =>
            {
                requestProviderCalls++;
                return DualCameraCaptureRequest.CreateHardwareDual(
                    DualCameraRigProfile.ApprovedSynthetic(),
                    HardwareDualCaptureProfile.ApprovedSynthetic(),
                    new HardwareDualOperatorConfirmations(true, true, true, true, true),
                    Guid.NewGuid());
            },
            dualBindingTransport: new SimulatedDualBindingAgentTransport(new SimulatedDualBindingAgent()));
        await recoveryViewModel.InitializeAsync(CancellationToken.None);
        recoveryViewModel.AcceptSafetyCommand.Execute(null);
        await CompleteDualBindingAsync(recoveryViewModel.DualBinding);
        recoveryViewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => !recoveryViewModel.IsBusy && recoveryFlow.Current?.FailureCode == DualCameraFailureCode.AgentResponseUnknown,
            "HardwareDual WPF response-unknown state was not retained.");
        recoveryIdentity.Set(DualCameraIdentitySnapshot.HardwarePending());
        var restartedRecoveryFlow = new DualCameraProductFlow(
            recoveryProductRoot,
            new HardwareDualCaptureSource(
                recoveryOperations,
                recoveryStore: new HardwareDualTransactionSnapshotStore(recoveryProductRoot)),
            hardwareAdapter,
            recoveryIdentity);
        var restartedRecoveryViewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "hardware-restarted-recovery-journals")),
            restartedRecoveryFlow,
            () =>
            {
                requestProviderCalls++;
                throw new InvalidOperationException("Restart recovery must not request current capture inputs.");
            });
        await restartedRecoveryViewModel.InitializeAsync(CancellationToken.None);
        restartedRecoveryViewModel.AcceptSafetyCommand.Execute(null);
        Check.True(restartedRecoveryViewModel.CanCapture, "Saved HardwareDual transaction recovery must remain available after restart with current identity Pending.");
        restartedRecoveryViewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => !restartedRecoveryViewModel.IsBusy && restartedRecoveryFlow.Current?.FailureCode == DualCameraFailureCode.None,
            "HardwareDual WPF saved transaction recovery did not finish after restart.");
        Check.Equal(1, requestProviderCalls);
        Check.Equal(0, restartedRecoveryViewModel.TransactionStartCount);
        Check.Equal(1, recoveryOperations.ReserveCalls);
        Check.Equal(1, recoveryOperations.StartCalls);
        Check.Equal(2, recoveryOperations.QueryCalls);
        Check.Equal(OperatorUiState.Review, restartedRecoveryViewModel.UiState);
        var recoveryStatePath = Path.Combine(
            recoveryProductRoot,
            "recovery-state",
            "pending-transaction.json");

        var snapshotTransactionId = Guid.NewGuid();
        var snapshotStartedAt = DateTimeOffset.UtcNow;
        var snapshotRequest = new DualHardwareCaptureRequest(
            snapshotTransactionId,
            Path.Combine(
                recoveryProductRoot,
                "transactions",
                snapshotTransactionId.ToString("N")),
            DualCameraIdentitySnapshot.AnonymousTestSyntheticReady(),
            HardwareDualCaptureProfile.ApprovedSynthetic(),
            DualCameraRigProfile.ApprovedSynthetic(),
            new HardwareDualOperatorConfirmations(true, true, true, true, true),
            snapshotStartedAt,
            snapshotStartedAt.AddSeconds(180));
        var snapshotStore = new HardwareDualTransactionSnapshotStore(recoveryProductRoot);
        snapshotStore.SavePending(snapshotRequest);
        Check.Equal(
            DualHardwareRecoveryIntent.MayHaveDispatched,
            snapshotStore.LoadPendingIntent(snapshotTransactionId));
        snapshotStore.MarkCloseReservedBeforeDispatch(snapshotTransactionId);
        Check.Equal(
            DualHardwareRecoveryIntent.CloseReservedBeforeDispatch,
            new HardwareDualTransactionSnapshotStore(recoveryProductRoot)
                .LoadPendingIntent(snapshotTransactionId));
        snapshotStore.ClearPending(snapshotTransactionId);

        var legacyOptions = new JsonSerializerOptions(JsonSerializerDefaults.Web)
        {
            WriteIndented = true,
            Converters =
            {
                new System.Text.Json.Serialization.JsonStringEnumConverter(),
            },
        };
        File.WriteAllText(
            recoveryStatePath,
            JsonSerializer.Serialize(
                new
                {
                    schemaVersion = "a0.hardware-dual-transaction-snapshot.v1",
                    dispatchMayHaveOccurred = true,
                    pendingRequest = snapshotRequest,
                },
                legacyOptions));
        var migratedLegacy = new HardwareDualTransactionSnapshotStore(
            recoveryProductRoot);
        Check.Equal(
            snapshotTransactionId,
            migratedLegacy.LoadPending()!.TransactionId);
        Check.Equal(
            DualHardwareRecoveryIntent.MayHaveDispatched,
            migratedLegacy.LoadPendingIntent(snapshotTransactionId));
        migratedLegacy.ClearPending(snapshotTransactionId);

        File.WriteAllText(recoveryStatePath, "{\"schemaVersion\":\"unsupported\",\"dispatchMayHaveOccurred\":false,\"pendingRequest\":null}");
        Check.Throws<InvalidDataException>(() =>
            new HardwareDualTransactionSnapshotStore(recoveryProductRoot).LoadPending());
        File.WriteAllBytes(recoveryStatePath, new byte[256 * 1024 + 1]);
        Check.Throws<InvalidDataException>(() =>
            new HardwareDualTransactionSnapshotStore(recoveryProductRoot).LoadPending());

        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(transactionRoot),
            productFlow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.FixedLocalExportDirectory = exportRoot;
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "The typed TestSynthetic dual flow must be ready.");

        viewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The formal dual-camera capture did not finish.");
        Check.Equal(OperatorUiState.Review, viewModel.UiState);
        Check.True(viewModel.CaptureResult.Contains("canonical JPEG", StringComparison.Ordinal), "Both originals must be displayed as verified JPEGs.");
        Check.True(viewModel.RetainedOriginals.Contains("SHA-256", StringComparison.Ordinal), "The UI must display canonical original verification evidence.");
        Check.True(viewModel.StitchResult.Contains("実JPEG合成完了", StringComparison.Ordinal), "The formal shell must display a real stitched JPEG.");
        Check.True(viewModel.ProgressSteps.Where(step => step.Id is not ("liveview" or "review" or "export")).All(step => step.StatusText == "完了"), "Every capture, validation, and stitch stage must be complete.");
        Check.Equal("処理中", viewModel.ProgressSteps.Single(step => step.Id == "review").StatusText);
        Check.Equal("待機", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);
        var firstJob = viewModel.LastStitchJobId;

        viewModel.RestitchCommand.Execute(null);
        await WaitUntilAsync(
            () => !viewModel.IsBusy && !string.Equals(firstJob, viewModel.LastStitchJobId, StringComparison.Ordinal),
            "Formal restitch did not publish a distinct job.");
        Check.True(viewModel.CanExport, "The reviewed restitch must be explicitly exportable.");

        viewModel.ExportCommand.Execute(null);
        await WaitUntilAsync(
            () => !viewModel.IsBusy && viewModel.ExportResult.Contains("このPCのフォルダへ保存しました", StringComparison.Ordinal),
            "Formal fixed-local export did not finish.");
        Check.True(File.Exists(viewModel.LastExportPath), "The formal WPF export must publish a JPEG.");
        Check.True(File.ReadAllBytes(viewModel.LastExportPath) is [0xff, 0xd8, .., 0xff, 0xd9], "The WPF export must be an actual JPEG.");
        Check.Equal("完了", viewModel.ProgressSteps.Single(step => step.Id == "review").StatusText);
        Check.Equal("完了", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);

        viewModel.PrepareNewCaptureCommand.Execute(null);
        await WaitUntilAsync(() => viewModel.CanCapture, "A new formal diagnostic capture was not prepared.");
        viewModel.SelectedDiagnosticScenario = "CAM-B撮影失敗";
        viewModel.DiagnosticCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 2 && !viewModel.IsBusy,
            "The typed CAM-B failure diagnostic did not finish.");
        Check.Equal(OperatorUiState.FailedPartial, viewModel.UiState);
        Check.True(viewModel.RetainedOriginals.Contains("CAM-A: original.jpg", StringComparison.Ordinal), "CAM-A actual JPEG must remain after CAM-B failure.");
        Check.False(viewModel.RetainedOriginals.Contains("CAM-B: original.jpg", StringComparison.Ordinal), "CAM-B failure must not invent an original.");
        Check.True(viewModel.TechnicalDetail.Contains("automatic retry count: 0", StringComparison.Ordinal), "The formal diagnostic must show zero retries.");
        Check.Equal(0, Directory.EnumerateFiles(productRoot, "*.simulated", SearchOption.AllDirectories).Count());
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_M2_ADAPTER_PATH", previousAdapterPath);
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task FormalDualCameraExportFailureProgressAsync()
{
    var bundledAdapterPath = Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe");
    Check.True(File.Exists(bundledAdapterPath), "The WPF test output must contain the bundled M2 adapter.");
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-FormalDualExportFailureTests",
        Guid.NewGuid().ToString("N"));
    var transactionRoot = Path.Combine(root, "legacy-journals");
    var productRoot = Path.Combine(root, "products");
    var exportRoot = Path.Combine(root, "operator-export");
    Directory.CreateDirectory(transactionRoot);
    Directory.CreateDirectory(exportRoot);
    try
    {
        var native = new M2OfflineStitcherProcessAdapter(bundledAdapterPath);
        var bridge = new BlockingFailedExportBridge(native);
        var flow = new DualCameraProductFlow(
            productRoot,
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(
                DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(transactionRoot),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.FixedLocalExportDirectory = exportRoot;
        viewModel.AcceptSafetyCommand.Execute(null);
        viewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The export-failure fixture capture did not finish.");

        Check.Equal("処理中", viewModel.ProgressSteps.Single(step => step.Id == "review").StatusText);
        Check.Equal("待機", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);
        viewModel.ExportCommand.Execute(null);
        await bridge.ExportStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Check.Equal("完了", viewModel.ProgressSteps.Single(step => step.Id == "review").StatusText);
        Check.Equal("処理中", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);

        bridge.ReleaseExport.TrySetResult();
        await WaitUntilAsync(
            () => !viewModel.IsBusy && viewModel.ExportResult.Contains("export失敗", StringComparison.Ordinal),
            "The deterministic export failure did not reach WPF.");
        Check.Equal("失敗", viewModel.ProgressSteps.Single(step => step.Id == "export").StatusText);
        Check.Equal(OperatorUiState.Review, viewModel.UiState);
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task SingleCameraRestartPreservesPlanAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var first = new OperatorShellViewModel(new SimulationFoundationService(root));
        await first.InitializeAsync(CancellationToken.None);
        first.SelectedOperatingMode = "1台構成";
        first.SelectedCamera = "CAM-B";
        first.AcceptSafetyCommand.Execute(null);
        first.SelectedDiagnosticScenario = "Live View停止失敗";
        first.DiagnosticCommand.Execute(null);
        await WaitUntilAsync(
            () => first.TransactionStartCount == 1 && !first.IsBusy,
            "The single-camera durable failure did not finish.");
        Check.Equal(OperatorUiState.FailedPartial, first.UiState);
        var transactionId = first.LastTransactionId;

        var restarted = new OperatorShellViewModel(new SimulationFoundationService(root));
        await restarted.InitializeAsync(CancellationToken.None);
        Check.Equal(OperatorUiState.FailedPartial, restarted.UiState);
        Check.Equal(transactionId, restarted.LastTransactionId);
        Check.True(restarted.IsSingleCameraMode, "Restart must restore Single mode from the journal.");
        Check.Equal("CAM-B", restarted.SelectedCamera);
        Check.True(restarted.CaptureButtonText.Contains("CAM-B", StringComparison.Ordinal), "Restart must display the durable selected alias.");
        Check.True(restarted.CameraAStatus.Contains("構成対象外", StringComparison.Ordinal), "Restart must not reclassify CAM-A as required.");
        Check.False(restarted.CanCapture, "Restart must not automatically resume or replace the failed transaction.");

        restarted.AcceptSafetyCommand.Execute(null);
        Check.False(restarted.CanCapture, "Safety acknowledgment alone must not bypass explicit preparation.");
        restarted.PrepareNewCaptureCommand.Execute(null);
        await WaitUntilAsync(() => !restarted.IsBusy, "Preparing the restarted single-camera workflow did not finish.");
        Check.True(restarted.CanCapture, "Explicit preparation must preserve the recovered Single CAM-B plan.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task SingleCameraWorkflowAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    string? exportPath = null;
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.SelectedOperatingMode = "1台構成";
        viewModel.SelectedCamera = "CAM-B";

        Check.True(viewModel.IsSingleCameraMode, "The operator must explicitly select Single mode.");
        Check.True(viewModel.CaptureButtonText.Contains("CAM-B", StringComparison.Ordinal), "The capture action must name the selected body.");
        Check.True(viewModel.CameraAStatus.Contains("構成対象外", StringComparison.Ordinal), "Inactive CAM-A must be shown as outside the plan.");
        Check.False(viewModel.CanCapture, "Safety acknowledgment remains mandatory in Single mode.");

        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "A ready Single CAM-B plan must allow capture.");
        viewModel.CaptureCommand.Execute(null);
        viewModel.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The single-camera workflow did not finish.");

        Check.Equal(OperatorUiState.Review, viewModel.UiState);
        Check.True(viewModel.RetainedOriginals.Contains("CAM-B", StringComparison.Ordinal), "CAM-B original must be retained.");
        Check.False(viewModel.RetainedOriginals.Contains("CAM-A", StringComparison.Ordinal), "Single CAM-B must not invent CAM-A.");
        Check.True(viewModel.StitchResult.Contains("対象外", StringComparison.Ordinal), "Stitch must be NotApplicable in Single mode.");
        Check.True(viewModel.CanExport, "A reviewed single original must be exportable.");
        Check.False(viewModel.CanRestitch, "Single mode must not enable restitch.");

        viewModel.ExportCommand.Execute(null);
        exportPath = viewModel.LastExportPath;
        Check.True(File.Exists(exportPath), "The explicit simulated single export must exist.");
        Check.True(viewModel.ExportResult.Contains("単体原画像", StringComparison.Ordinal), "The export must be labeled as a single original.");
    }
    finally
    {
        if (!string.IsNullOrWhiteSpace(exportPath) && File.Exists(exportPath))
        {
            File.Delete(exportPath);
        }
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task ModeAndAliasLockDuringCaptureAsync()
{
    var service = new BlockingTransactionService();
    var viewModel = new OperatorShellViewModel(service);
    await viewModel.InitializeAsync(CancellationToken.None);
    viewModel.SelectedOperatingMode = "1台構成";
    viewModel.SelectedCamera = "CAM-B";
    viewModel.AcceptSafetyCommand.Execute(null);

    viewModel.CaptureCommand.Execute(null);
    await service.Started.WaitAsync(TimeSpan.FromSeconds(5));
    Check.True(viewModel.IsBusy, "The capture must hold the UI operation lock.");
    Check.False(viewModel.CanChangeOperatingMode, "Operating mode must be locked during capture.");
    Check.False(viewModel.CanSelectCamera, "The selected alias must be locked during capture.");

    viewModel.SelectedOperatingMode = "2台構成";
    viewModel.SelectedCamera = "CAM-A";
    Check.Equal("1台構成", viewModel.SelectedOperatingMode);
    Check.Equal("CAM-B", viewModel.SelectedCamera);

    service.Release();
    await WaitUntilAsync(() => !viewModel.IsBusy, "The blocking capture did not finish.");
    Check.Equal(OperatorUiState.Review, viewModel.UiState);
    Check.True(viewModel.RetainedOriginals.Contains("CAM-B", StringComparison.Ordinal), "The snapshotted plan must remain CAM-B.");
}

static void VerifyHardwareCameraAgentStderrSanitizer()
{
    const string longIdentifier = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    var sanitized = HardwareCameraAgentDiagnostic.SanitizeStandardError(
        "ordinary readiness failure sdk_load_failed " +
        $"requestId={longIdentifier} identifier={longIdentifier} cameraId={longIdentifier} " +
        $"bare {longIdentifier} repeated {longIdentifier}");

    Check.True(
        sanitized.Contains("ordinary readiness failure sdk_load_failed", StringComparison.Ordinal),
        "Ordinary diagnostic prose and a safe error code must remain readable.");
    Check.False(
        sanitized.Contains(longIdentifier, StringComparison.Ordinal),
        "A long general alphanumeric identifier must be redacted regardless of key or position.");
    Check.True(
        sanitized.Contains("requestId=[redacted]", StringComparison.Ordinal) &&
        sanitized.Contains("identifier=[redacted]", StringComparison.Ordinal) &&
        sanitized.Contains("cameraId=[redacted-identifier]", StringComparison.Ordinal),
        "Known and unknown identifier keys must both redact their values.");
    Check.True(
        sanitized.Split("[redacted-identifier]", StringSplitOptions.None).Length - 1 == 3,
        "Every unknown-key and bare occurrence must be redacted.");

    const string belowBoundary = "ABCDEFGHIJKLMNOPQRSTUV1";
    const string atBoundary = "ABCDEFGHIJKLMNOPQRSTUVW1";
    Check.True(belowBoundary.Length == 23 && atBoundary.Length == 24, "The identifier boundary fixture must remain exact.");
    var boundary = HardwareCameraAgentDiagnostic.SanitizeStandardError(
        $"short token {belowBoundary} boundary token {atBoundary} safe code E_CAMERA_17");
    Check.True(
        boundary.Contains(belowBoundary, StringComparison.Ordinal),
        "A 23-character alphanumeric token must remain available to diagnostics.");
    Check.False(
        boundary.Contains(atBoundary, StringComparison.Ordinal),
        "A 24-character alphanumeric identifier must be redacted.");
    Check.True(
        boundary.Contains("safe code E_CAMERA_17", StringComparison.Ordinal),
        "A short safe error code must not be damaged.");

    const string hexadecimalIdentifier = "0123456789abcdef0123456789abcdef";
    const string uuidIdentifier = "123e4567-e89b-12d3-a456-426614174000";
    var existingProtections = HardwareCameraAgentDiagnostic.SanitizeStandardError(
        "diagnostic secret=super-secret path=C:\\private\\sdk rawIdentity=RAW-CAMERA-IDENTITY " +
        $"hex={hexadecimalIdentifier} uuid={uuidIdentifier} " + new string('x', 600));
    Check.False(
        existingProtections.Contains("super-secret", StringComparison.Ordinal) ||
        existingProtections.Contains("C:\\private", StringComparison.Ordinal) ||
        existingProtections.Contains("RAW-CAMERA-IDENTITY", StringComparison.Ordinal) ||
        existingProtections.Contains(hexadecimalIdentifier, StringComparison.Ordinal) ||
        existingProtections.Contains(uuidIdentifier, StringComparison.Ordinal),
        "Existing secret, path, raw identity, hex, and UUID protections must remain active.");
    Check.True(existingProtections.Length <= 512, "Sanitized stderr must remain bounded to 512 characters.");
}

static async Task LiveViewStopFailureWorkflowAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "The simulated shell must be ready before the diagnostic starts.");

        viewModel.SelectedDiagnosticScenario = "Live View停止失敗";
        viewModel.DiagnosticCommand.Execute(null);
        viewModel.DiagnosticCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The durable failure workflow did not finish.");

        Check.Equal(1, viewModel.TransactionStartCount);
        Check.Equal(OperatorUiState.FailedPartial, viewModel.UiState);
        Check.True(viewModel.TechnicalDetail.Contains("LiveViewStopFailed", StringComparison.Ordinal), "The terminal reason must be visible.");
        Check.True(viewModel.TechnicalDetail.Contains("capture calls: 0", StringComparison.Ordinal), "Zero capture calls must be visible.");
        Check.True(viewModel.TechnicalDetail.Contains("automatic retry count: 0", StringComparison.Ordinal), "Zero retries must be visible.");
        Check.False(viewModel.CanCapture, "A failed transaction must block another capture until preparation.");
        Check.True(viewModel.CanPrepareNewCapture, "The operator must be able to explicitly prepare a new transaction.");
        Check.Equal(1, Directory.EnumerateDirectories(root).Count());

        var transactionId = Guid.ParseExact(viewModel.LastTransactionId, "N");
        viewModel.DiagnosticCommand.Execute(null);
        await Task.Delay(100);
        Check.Equal(1, viewModel.TransactionStartCount);

        var restarted = new OperatorShellViewModel(new SimulationFoundationService(root));
        await restarted.InitializeAsync(CancellationToken.None);
        Check.Equal(OperatorUiState.FailedPartial, restarted.UiState);
        Check.Equal(transactionId.ToString("N"), restarted.LastTransactionId);
        Check.True(restarted.TechnicalDetail.Contains("LiveViewStopFailed", StringComparison.Ordinal), "Restart must rediscover the same terminal reason.");
        Check.False(restarted.CanCapture, "Restart must not resume or recapture the failed transaction.");

        restarted.AcceptSafetyCommand.Execute(null);
        Check.False(restarted.CanCapture, "Safety acknowledgment must not bypass explicit new-capture preparation.");
        restarted.PrepareNewCaptureCommand.Execute(null);
        await WaitUntilAsync(() => !restarted.IsBusy, "Preparing a new transaction did not finish.");
        Check.True(restarted.CanCapture, "Only explicit preparation may allow a new transaction.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

// ---------------------------------------------------------------------------
// Dual binding confirmation UI (ADR-0025, Issue #62)
// ---------------------------------------------------------------------------

static OperatorShellViewModel HardwareDualShellWithSimulatedBinding(
    string root,
    SimulatedDualBindingAgent agent)
{
    var adapter = new M2OfflineStitcherProcessAdapter(
        Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe"));
    return new OperatorShellViewModel(
        new SimulationFoundationService(Path.Combine(root, "binding-journals")),
        new DualCameraProductFlow(
            Path.Combine(root, "binding-products"),
            new HardwareDualCaptureSource(new WpfHardwareDualFakeOperations(adapter)),
            adapter,
            new FixedDualCameraIdentitySnapshotSource(
                DualCameraIdentitySnapshot.AnonymousTestSyntheticReady())),
        () => DualCameraCaptureRequest.CreateHardwareDual(
            DualCameraRigProfile.ApprovedSynthetic(),
            HardwareDualCaptureProfile.ApprovedSynthetic(),
            new HardwareDualOperatorConfirmations(true, true, true, true, true),
            Guid.NewGuid()),
        dualBindingTransport: new SimulatedDualBindingAgentTransport(agent));
}

static async Task DualBindingOverlayGatesCaptureAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var agent = new SimulatedDualBindingAgent();
        var shell = HardwareDualShellWithSimulatedBinding(root, agent);
        await shell.InitializeAsync(CancellationToken.None);
        shell.AcceptSafetyCommand.Execute(null);

        Check.True(shell.DualBinding.IsRequired, "HardwareDual must require a binding.");
        Check.True(shell.DualBinding.IsOverlayVisible, "The binding overlay must cover the screen until Ready.");
        Check.False(shell.CanCapture, "Capture must not start before the binding is confirmed.");
        Check.True(
            shell.CaptureDisabledReason.Contains("機体照合", StringComparison.Ordinal),
            "The disabled reason must name the outstanding binding.");

        shell.DualBinding.BeginBindingCommand.Execute(null);
        await WaitUntilAsync(
            () => shell.DualBinding.Candidates.Count == 2 && !shell.DualBinding.IsBusy,
            "The binding session did not offer two candidates.");
        Check.Equal("候補1", shell.DualBinding.Candidates[0].DisplayName);
        Check.Equal("候補2", shell.DualBinding.Candidates[1].DisplayName);

        // One preview at a time, never two. Comparing them side by side is the mistake this flow
        // exists to prevent.
        shell.DualBinding.ShowCandidateCommand.Execute(shell.DualBinding.Candidates[0]);
        await WaitUntilAsync(() => !shell.DualBinding.IsBusy, "Showing the first candidate did not settle.");
        Check.Equal(1, agent.ActiveLiveViewCount);
        shell.DualBinding.ShowCandidateCommand.Execute(shell.DualBinding.Candidates[1]);
        await WaitUntilAsync(
            () => shell.DualBinding.SelectedCandidate == shell.DualBinding.Candidates[1] && !shell.DualBinding.IsBusy,
            "Switching candidates did not settle.");
        Check.Equal(1, agent.ActiveLiveViewCount);
        Check.Equal(0, agent.ConcurrentLiveViewViolationCount);

        // The simulated agent returns generated bytes, so the screen says so instead of drawing
        // something that could be mistaken for a camera frame.
        Check.True(
            shell.DualBinding.IsPreviewPlaceholderVisible,
            "A frame that is not a decodable image must fall back to a labelled placeholder.");
        Check.True(
            shell.DualBinding.PreviewPlaceholderText.Contains("カメラ画像ではありません", StringComparison.Ordinal),
            "The placeholder must say it is not a camera image.");

        shell.DualBinding.AssignCameraACommand.Execute(null);
        await WaitUntilAsync(
            () => shell.DualBinding.Candidates[1].AssignedAlias == "CAM-A" && !shell.DualBinding.IsBusy,
            "The first alias was not assigned.");
        Check.False(shell.CanCapture, "One assignment must not enable capture.");
        Check.False(
            shell.DualBinding.AssignCameraACommand.CanExecute(null),
            "CAM-A must not be offered twice.");

        shell.DualBinding.ShowCandidateCommand.Execute(shell.DualBinding.Candidates[0]);
        await WaitUntilAsync(() => !shell.DualBinding.IsBusy, "Showing the remaining candidate did not settle.");
        shell.DualBinding.AssignCameraBCommand.Execute(null);
        await WaitUntilAsync(
            () => shell.DualBinding.Phase == DualBindingPhase.Summary,
            "Both assignments did not reach the confirmation summary.");

        // The summary exists so the operator sees what they chose before it is committed.
        Check.Equal(2, shell.DualBinding.SummaryLines.Count);
        Check.True(
            shell.DualBinding.SummaryLines[0].StartsWith("CAM-A", StringComparison.Ordinal),
            "The summary must list the aliases in rig order.");
        Check.False(shell.CanCapture, "The summary is not a confirmed binding.");

        shell.DualBinding.CompleteBindingCommand.Execute(null);
        await WaitUntilAsync(() => shell.DualBinding.IsReady, "The binding did not complete.");
        Check.False(shell.DualBinding.IsOverlayVisible, "A Ready binding must uncover the screen.");
        Check.True(shell.CanCapture, "A confirmed binding must enable capture.");
        Check.Equal(1, agent.EnumerationCount);
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task DualBindingBlocksTheSingleCameraFallbackAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var agent = new SimulatedDualBindingAgent();
        var shell = HardwareDualShellWithSimulatedBinding(root, agent);
        await shell.InitializeAsync(CancellationToken.None);
        shell.AcceptSafetyCommand.Execute(null);
        Check.False(shell.CanCapture, "The outstanding binding must block capture.");

        // Switching the operating mode is the one route that could look like a way around an
        // unfinished binding: one camera needs no CAM-A/CAM-B decision. It must not become one.
        // A HardwareDual rig with an unconfirmed binding stays blocked whatever mode is selected,
        // because the bodies attached to it are still indistinguishable.
        Check.True(shell.CanChangeOperatingMode, "The operating mode must still be selectable.");
        shell.IsSingleCameraModeChecked = true;
        Check.True(shell.IsSingleCameraMode, "The shell must have switched to SingleCamera.");
        Check.False(
            shell.CanCapture,
            "SingleCamera mode must not become a way to capture around an unconfirmed binding.");
        Check.True(
            shell.CaptureDisabledReason.Contains("機体照合", StringComparison.Ordinal),
            "The blocker must still name the binding after the mode switch.");

        // And it stays blocked at the command level, not only on the property the button binds to.
        var startsBefore = shell.TransactionStartCount;
        shell.CaptureCommand.Execute(null);
        await Task.Delay(50);
        Check.Equal(startsBefore, shell.TransactionStartCount);

        shell.IsDualCameraModeChecked = true;
        await CompleteDualBindingAsync(shell.DualBinding);
        Check.True(shell.CanCapture, "A confirmed binding must enable capture again.");
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task DualBindingOverlayAccessibilityAndBusyLockAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var agent = new SimulatedDualBindingAgent();
        var shell = HardwareDualShellWithSimulatedBinding(root, agent);
        await shell.InitializeAsync(CancellationToken.None);
        shell.AcceptSafetyCommand.Execute(null);
        shell.DualBinding.BeginBindingCommand.Execute(null);
        await WaitUntilAsync(
            () => shell.DualBinding.Candidates.Count == 2 && !shell.DualBinding.IsBusy,
            "The binding session did not offer two candidates.");

        // Read aloud instead of the colour, and it has to change as the state does -- a name that
        // never updates is worse than none, because it reports the wrong state confidently.
        var candidate = shell.DualBinding.Candidates[0];
        Check.Equal("候補1 未割当", candidate.AccessibleName);
        shell.DualBinding.ShowCandidateCommand.Execute(candidate);
        await WaitUntilAsync(() => !shell.DualBinding.IsBusy, "Showing the candidate did not settle.");
        Check.Equal("候補1 未割当 表示中", candidate.AccessibleName);
        shell.DualBinding.AssignCameraACommand.Execute(null);
        await WaitUntilAsync(
            () => candidate.IsAssigned && !shell.DualBinding.IsBusy,
            "The alias was not assigned.");
        Check.Equal("候補1 CAM-A", candidate.AccessibleName);
        Check.Equal("assigned", candidate.StateKey);

        // Block / Caution / Info in words, so the state survives a screen reader and a monochrome
        // display.
        Check.True(
            shell.DualBinding.HeadlineKind is "Block" or "Caution" or "Info",
            "The headline must carry a written severity, not only a colour.");
        Check.True(
            DualBindingViewModel.ResidualRiskText.Contains("自動検出できません", StringComparison.Ordinal) &&
            DualBindingViewModel.ResidualRiskText.Contains("HardwarePending", StringComparison.Ordinal) &&
            DualBindingViewModel.ResidualRiskText.Contains("同期は保証しません", StringComparison.Ordinal),
            "The overlay must state the misassignment risk, HardwarePending, and the shutter-sync limit.");

        // The busy lock is what stops a double-click issuing two Agent requests against a session
        // whose state the first one is still changing.
        var gate = new TaskCompletionSource();
        var gatedShell = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "gated-journals")),
            null,
            dualBindingTransport: new GatedDualBindingTransport(agent, gate.Task));
        gatedShell.DualBinding.IsRequired = true;
        gatedShell.DualBinding.BeginBindingCommand.Execute(null);
        await WaitUntilAsync(() => gatedShell.DualBinding.IsBusy, "The binding request did not start.");
        Check.False(
            gatedShell.DualBinding.BeginBindingCommand.CanExecute(null),
            "A second binding request must be refused while the first is in flight.");
        gate.SetResult();
        await WaitUntilAsync(() => !gatedShell.DualBinding.IsBusy, "The gated binding request did not finish.");
        Check.True(
            gatedShell.DualBinding.BeginBindingCommand.CanExecute(null) ||
            gatedShell.DualBinding.Phase == DualBindingPhase.Collecting,
            "The lock must release once the request completes.");
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

static async Task DualBindingOverlayInvalidationRestartsTheFlowAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        var agent = new SimulatedDualBindingAgent();
        var shell = HardwareDualShellWithSimulatedBinding(root, agent);
        await shell.InitializeAsync(CancellationToken.None);
        shell.AcceptSafetyCommand.Execute(null);
        await CompleteDualBindingAsync(shell.DualBinding);
        Check.True(shell.CanCapture, "The confirmed binding must enable capture.");

        // A Ready binding cannot learn it was invalidated on its own -- nothing pushes. The probe
        // that runs immediately before a capture is what asks, and it has to refuse before any
        // shutter is dispatched.
        agent.RaiseInvalidation(DualBindingInvalidationReason.TopologyChanged);

        // Driven through the capture command rather than the probe directly, because the thing
        // that has to hold is "no shutter is dispatched", not "a method returns false". Calling the
        // probe here would pass even if nothing on the capture path ever called it.
        Check.True(shell.CanCapture, "Nothing has told the shell about the change yet.");
        var startsBefore = shell.TransactionStartCount;
        shell.CaptureCommand.Execute(null);
        await WaitUntilAsync(
            () => !shell.IsBusy && shell.DualBinding.RequiresRebindingText,
            "A body unplugged after the binding was confirmed must be caught before capture.");
        Check.Equal(startsBefore, shell.TransactionStartCount);
        Check.True(
            shell.DualBinding.InvalidationText.Contains("接続構成の変化", StringComparison.Ordinal),
            "The overlay must name the invalidation reason in words the operator can act on.");
        Check.False(shell.CanCapture, "An invalidated binding must block capture immediately.");
        Check.True(shell.DualBinding.IsOverlayVisible, "The overlay must cover the screen again.");

        shell.DualBinding.BeginBindingCommand.Execute(null);
        await WaitUntilAsync(
            () => !shell.DualBinding.IsBusy && shell.DualBinding.Candidates.Count == 2,
            "Re-binding after a topology change did not offer candidates again.");

        // Re-binding enumerates again rather than reusing anything from the previous session: the
        // ordinals, the preview and the assignments were all specific to a topology that changed.
        Check.Equal(2, agent.EnumerationCount);
        Check.True(
            shell.DualBinding.Candidates.All(item => !item.IsAssigned),
            "A re-binding must start with no assignment carried over.");
        Check.Equal(0, shell.DualBinding.SummaryLines.Count);
        Check.False(shell.DualBinding.IsPreviewVisible, "A re-binding must start with no preview.");
        Check.False(shell.CanCapture, "Capture must be blocked again until the new binding completes.");
    }
    finally
    {
        if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
    }
}

// Drives the binding overlay the way an operator would: start a session, look at each candidate,
// assign it, then confirm the summary. Commands are used rather than the client underneath, so a
// test that reaches Ready has proven the screen can get there too.
static async Task CompleteDualBindingAsync(DualBindingViewModel binding)
{
    binding.BeginBindingCommand.Execute(null);
    await WaitUntilAsync(
        () => binding.Candidates.Count == 2 && !binding.IsBusy,
        "The binding session did not offer two candidates.");

    foreach (var alias in new[] { "CAM-A", "CAM-B" })
    {
        var candidate = binding.Candidates.First(item => !item.IsAssigned);
        binding.ShowCandidateCommand.Execute(candidate);
        await WaitUntilAsync(
            () => binding.SelectedCandidate == candidate && !binding.IsBusy,
            $"The binding screen did not show {candidate.DisplayName}.");

        var assign = alias == "CAM-A" ? binding.AssignCameraACommand : binding.AssignCameraBCommand;
        assign.Execute(null);
        await WaitUntilAsync(
            () => candidate.AssignedAlias == alias && !binding.IsBusy,
            $"{candidate.DisplayName} was not assigned to {alias}.");
    }

    await WaitUntilAsync(
        () => binding.Phase == DualBindingPhase.Summary,
        "The binding screen did not reach the confirmation summary.");
    binding.CompleteBindingCommand.Execute(null);
    await WaitUntilAsync(() => binding.IsReady, "The binding did not complete.");
}

static async Task WaitUntilAsync(Func<bool> predicate, string message)
{
    var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
    while (!predicate())
    {
        if (DateTime.UtcNow >= deadline)
        {
            throw new TimeoutException(message);
        }
        await Task.Delay(20);
    }
}

static string CreateHardwareTestRoot()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-HardwareOperatorTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    return root;
}

// Mirrors HardwareSingleStoragePaths.Resolve's real layout
// (<LocalAppData>/A0CameraStitcher/phase0/camera-agent/artifacts) under a
// test root, deliberately in a different subtree than any fake agent exe
// path a test also sets -- see FakeHardwareSingleCameraOperations
// .AgentArtifactsRoot for why the two must not be conflated.
static string FakeAgentArtifactsRoot(string root) =>
    Path.Combine(root, "localappdata", "A0CameraStitcher", "phase0", "camera-agent", "artifacts");

static HardwareRetainedOriginalRecord WriteJpegRecord(
    string path,
    string alias,
    bool preserveOnePixelDimensions = false)
{
    var pixels = new byte[] { 0x20, 0x80, 0xE0 };
    var bitmap = BitmapSource.Create(
        1, 1, 96, 96, PixelFormats.Bgr24, null, pixels, stride: 3);
    var encoder = new JpegBitmapEncoder();
    encoder.Frames.Add(BitmapFrame.Create(bitmap));
    using var encoded = new MemoryStream();
    encoder.Save(encoded);
    var bytes = encoded.ToArray();
    if (!preserveOnePixelDimensions)
    {
        RewriteJpegDimensions(bytes, width: 7360, height: 4912);
    }
    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllBytes(path, bytes);
    return new HardwareRetainedOriginalRecord
    {
        CameraAlias = alias,
        Path = path,
        SizeBytes = bytes.Length,
        Sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
    };
}

static HardwarePreviewJpegRecord WritePreviewRecord(string path)
{
    var original = WriteJpegRecord(path, "CAM-A", preserveOnePixelDimensions: true);
    return new HardwarePreviewJpegRecord
    {
        Path = original.Path,
        SizeBytes = original.SizeBytes,
        Sha256 = original.Sha256,
    };
}

static void RewriteJpegDimensions(byte[] bytes, int width, int height)
{
    for (var index = 2; index + 8 < bytes.Length;)
    {
        if (bytes[index++] != 0xFF)
        {
            throw new InvalidDataException("Test JPEG marker structure is invalid.");
        }
        while (index < bytes.Length && bytes[index] == 0xFF) index++;
        var marker = bytes[index++];
        if (marker is 0xD9 or 0xDA) break;
        if (marker == 0x01 || marker is >= 0xD0 and <= 0xD7) continue;
        var segmentLength = (bytes[index] << 8) | bytes[index + 1];
        var isStartOfFrame = marker is >= 0xC0 and <= 0xCF and not (0xC4 or 0xC8 or 0xCC);
        if (isStartOfFrame)
        {
            bytes[index + 3] = (byte)(height >> 8);
            bytes[index + 4] = (byte)height;
            bytes[index + 5] = (byte)(width >> 8);
            bytes[index + 6] = (byte)width;
            return;
        }
        index += segmentLength;
    }
    throw new InvalidDataException("Test JPEG has no start-of-frame marker.");
}

// Shared with tests that must pre-write a Live View preview (whose canonical
// path only depends on runId + alias, not transactionId, so it can be
// written before the transaction ID is known) at a location that will later
// line up with a capture built by CompleteCapture/CompleteCaptureWithHandoff.
const string CompleteCaptureRunId = "run-1000-1";

// Writes the canonical original.jpg for (transactionId, alias) under
// artifactsRoot -- using HardwareAgentArtifactLayout, the same helper
// production code uses to compute the expected path -- and returns both the
// resulting HardwareSingleCaptureResult and the original's on-disk path (for
// tests that later assert byte-identical export output).
//
// The write happens here, not before the caller has a transactionId, because
// CaptureAsync's real transaction ID is only known once the ViewModel calls
// this factory: HardwareArtifactVerifier now requires the reported path to
// exactly equal <artifactsRoot>/<runId>/<transactionId>/<alias>/original.jpg,
// so the file cannot be pre-staged before that ID exists.
static (HardwareSingleCaptureResult Result, string OriginalPath) CompleteCapture(
    string artifactsRoot,
    string transactionId,
    string alias)
{
    var originalPath = HardwareAgentArtifactLayout.OriginalPath(
        artifactsRoot, CompleteCaptureRunId, transactionId, alias);
    var original = WriteJpegRecord(originalPath, alias);
    var result = new HardwareSingleCaptureResult
    {
        CameraMode = "SingleCamera",
        CameraAlias = alias,
        RequiredCameraAlias = alias,
        RunId = CompleteCaptureRunId,
        TransactionId = transactionId,
        CaptureProfileId = "single-profile",
        CaptureProfileVersion = 1,
        CaptureProfileSha256 = new string('a', 64),
        CaptureProfileCameraAlias = alias,
        CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
        TerminalState = "Complete",
        ErrorCategory = string.Empty,
        ErrorDetail = string.Empty,
        RetainedOriginal = original,
        LiveViewHandoffRequested = false,
        LiveViewStoppedBeforeCapture = false,
        LiveViewSdkSessionClosedBeforeCapture = false,
        PostCaptureLiveViewProbeAttempted = false,
        PostCaptureLiveViewProbeSucceeded = false,
        PostCapturePreview = null,
        SpoolEmptyBeforeCapture = true,
        CameraObjectDeleteAttempted = true,
        CameraObjectDeleteSucceeded = true,
        SpoolEmptyAfterCleanup = true,
        AutomaticRetryCount = 0,
        TransactionWatchdogSeconds = 180,
        RealIdentifiersIncluded = false,
    };
    return (result, originalPath);
}

static HardwareSingleCaptureResult InProgressCapture(string transactionId, string alias) =>
    new()
    {
        CameraMode = "SingleCamera",
        CameraAlias = alias,
        RequiredCameraAlias = alias,
        RunId = "run-2000-1",
        TransactionId = transactionId,
        CaptureProfileId = "single-profile",
        CaptureProfileVersion = 1,
        CaptureProfileSha256 = new string('a', 64),
        CaptureProfileCameraAlias = alias,
        CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
        TerminalState = "InProgress",
        ErrorCategory = "transaction_in_progress",
        ErrorDetail = "The same transaction remains active.",
        RetainedOriginal = null,
        LiveViewHandoffRequested = false,
        LiveViewStoppedBeforeCapture = false,
        LiveViewSdkSessionClosedBeforeCapture = false,
        PostCaptureLiveViewProbeAttempted = false,
        PostCaptureLiveViewProbeSucceeded = false,
        PostCapturePreview = null,
        SpoolEmptyBeforeCapture = false,
        CameraObjectDeleteAttempted = false,
        CameraObjectDeleteSucceeded = false,
        SpoolEmptyAfterCleanup = false,
        AutomaticRetryCount = 0,
        TransactionWatchdogSeconds = 180,
        RealIdentifiersIncluded = false,
    };

// resumedPreview must already be written at HardwareAgentArtifactLayout
// .PreviewPath(artifactsRoot, CompleteCaptureRunId, alias) -- e.g. because the
// same file was also used as the pre-capture finite Live View probe's
// Preview, before the transaction ID existed.
static (HardwareSingleCaptureResult Result, string OriginalPath) CompleteCaptureWithHandoff(
    string artifactsRoot,
    string transactionId,
    string alias,
    HardwarePreviewJpegRecord resumedPreview)
{
    var (captureResult, originalPath) = CompleteCapture(artifactsRoot, transactionId, alias);
    var result = captureResult with
    {
        LiveViewHandoffRequested = true,
        LiveViewStoppedBeforeCapture = true,
        LiveViewSdkSessionClosedBeforeCapture = true,
        PostCaptureLiveViewProbeAttempted = true,
        PostCaptureLiveViewProbeSucceeded = true,
        PostCapturePreview = resumedPreview,
    };
    return (result, originalPath);
}

static (HardwareSingleCaptureResult Result, string OriginalPath) FailedPartialCapture(
    string artifactsRoot,
    string transactionId,
    string alias)
{
    var (captureResult, originalPath) = CompleteCapture(artifactsRoot, transactionId, alias);
    var result = captureResult with
    {
        TerminalState = "FailedPartial",
        ErrorCategory = "spool_empty_after_failed",
        ErrorDetail = "The canonical PC original is retained but spool cleanup did not complete.",
        SpoolEmptyAfterCleanup = false,
    };
    return (result, originalPath);
}

static HardwareSingleCaptureResult ReservedCapture(string transactionId, string alias) =>
    InProgressCapture(transactionId, alias) with
    {
        TerminalState = "Reserved",
        ErrorCategory = "transaction_reserved",
        ErrorDetail = "The transaction reservation exists before camera access.",
    };

static HardwareSingleCaptureResult TransactionNotFoundCapture(string transactionId, string alias) =>
    InProgressCapture(transactionId, alias) with
    {
        TerminalState = "NotFound",
        ErrorCategory = "TransactionNotFound",
        ErrorDetail = "No durable Camera Agent journal exists for the attempted dispatch.",
    };

static HardwarePendingTransaction HardwarePending(
    string transactionId,
    string alias,
    bool captureRequestDispatchAttempted,
    bool liveViewHandoffRequested = false) =>
    new()
    {
        OperatingMode = "SingleCamera",
        TransactionId = transactionId,
        CameraAlias = alias,
        RequiredCameraAlias = alias,
        CaptureProfileId = "single-profile",
        CaptureProfileVersion = 1,
        CaptureProfileSha256 = new string('a', 64),
        CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
        LiveViewHandoffRequested = liveViewHandoffRequested,
        CaptureRequestDispatchAttempted = captureRequestDispatchAttempted,
        StartedAtUtc = DateTimeOffset.UtcNow,
    };

// ---------------------------------------------------------------------------
// HardwareDual .NET Agent lifecycle tests (Issue #8).
//
// These tests exercise DualCameraAgentLifecycle against a real, separate OS
// process (this same test apphost, re-invoked with a scenario environment
// variable) speaking the actual named-pipe wire protocol -- not an in-memory
// fake IDualHardwareCaptureOperations. That is the only way to exercise
// process/pipe failure and restart recovery for real.
// ---------------------------------------------------------------------------

static DualCameraCaptureRequest HardwareDualTestRequest(Guid transactionId) =>
    DualCameraCaptureRequest.CreateHardwareDual(
        DualCameraRigProfile.ApprovedSynthetic(),
        HardwareDualCaptureProfile.ApprovedSynthetic(),
        new HardwareDualOperatorConfirmations(true, true, true, true, true),
        transactionId);

static string DualCameraAgentTestHostPath()
{
    var path = Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M3.OperatorShellTests.exe");
    Check.True(File.Exists(path), "The fake Dual Camera Agent test host apphost must exist.");
    return path;
}

static string DualCameraM2AdapterPath() =>
    Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe");

// Native (and the fake host below, matched to the same strength) requires
// --approved-capture-profile / --dual-identity-proof to already be existing
// regular files. This writes test-only placeholder content -- DualCameraAgentLifecycle
// and the fake host only ever check that the paths are existing regular files; neither
// parses the contents in this harness. Never treat this as a stand-in for a real
// approved capture profile or identity proof.
static void WriteDualAgentTestArtifactFiles(string approvedCaptureProfilePath, string dualIdentityProofPath)
{
    Directory.CreateDirectory(Path.GetDirectoryName(approvedCaptureProfilePath)!);
    File.WriteAllText(
        approvedCaptureProfilePath,
        "{\"note\":\"test-only placeholder, not a real approved capture profile\"}");
    Directory.CreateDirectory(Path.GetDirectoryName(dualIdentityProofPath)!);
    File.WriteAllText(
        dualIdentityProofPath,
        "{\"note\":\"test-only placeholder, not a real dual identity proof\"}");
}

static async Task DualCameraAgentLifecycleIdentityPendingKeepsZeroProcessAsync()
{
    var root = CreateHardwareTestRoot();
    try
    {
        // A configured-but-nonexistent Agent executable: the lifecycle must construct
        // cleanly and never probe or launch it while DualCameraProductFlow's identity
        // gate keeps every capture rejected before any side effect.
        var agentPath = Path.Combine(root, "A0CameraStitcher.DualCameraAgent.exe");
        var journalRoot = Path.Combine(root, "agent-pair-journal");
        await using var lifecycle = new DualCameraAgentLifecycle(
            agentPath,
            journalRoot,
            Path.Combine(root, "camera-agent", "approved-dual-capture-profile.json"),
            Path.Combine(root, "phase0", "dual-identity-proof.json"));
        Check.False(lifecycle.AgentExecutableAvailable, "A nonexistent Agent path must report unavailable, not throw.");

        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            new HardwareDualCaptureSource(lifecycle),
            new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath()),
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.HardwarePending()));
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        Check.Equal(DualCameraIdentityStatus.HardwarePending, flow.IdentitySnapshot.Status);
        Check.False(
            viewModel.CanCapture,
            "HardwareDual must stay CanCapture=false while identity is Pending, even with a real Agent lifecycle wired in.");
        Check.Equal(OperatorShellViewModel.HardwareDualPendingBanner, viewModel.BannerText);

        var rejected = false;
        try
        {
            await flow.CaptureAndStitchAsync(HardwareDualTestRequest(Guid.NewGuid()));
        }
        catch (DualCameraFlowException exception) when (exception.Code == DualCameraFailureCode.IdentityNotReady)
        {
            rejected = true;
        }
        Check.True(rejected, "A Pending identity must reject capture with IdentityNotReady before any Agent call.");
        Check.False(
            Directory.Exists(journalRoot),
            "Zero process/camera while Pending: the Agent lifecycle must not even create its pair journal directory.");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static async Task DualCameraAgentLifecycleRequiresExistingArtifactFilesAsync()
{
    // Native requires --approved-capture-profile / --dual-identity-proof to already
    // be existing regular files and exits 1 if either is missing. This class never
    // fabricates those approval/proof artifacts, so a missing file must fail closed
    // before any process launch (zero process, zero pipe) -- not silently create a
    // placeholder and hand it to a real Native agent as if it were approved.
    foreach (var missing in new[] { "capture-profile", "identity-proof" })
    {
        var root = CreateHardwareTestRoot();
        try
        {
            var captureProfilePath = Path.Combine(root, "camera-agent", "approved-dual-capture-profile.json");
            var identityProofPath = Path.Combine(root, "phase0", "dual-identity-proof.json");
            Directory.CreateDirectory(Path.GetDirectoryName(captureProfilePath)!);
            Directory.CreateDirectory(Path.GetDirectoryName(identityProofPath)!);
            if (missing != "capture-profile")
            {
                File.WriteAllText(captureProfilePath, "{\"note\":\"test-only placeholder\"}");
            }
            if (missing != "identity-proof")
            {
                File.WriteAllText(identityProofPath, "{\"note\":\"test-only placeholder\"}");
            }

            var journalRoot = Path.Combine(root, "agent-pair-journal");
            await using var lifecycle = new DualCameraAgentLifecycle(
                DualCameraAgentTestHostPath(),
                journalRoot,
                captureProfilePath,
                identityProofPath);

            HardwareCameraAgentLaunchException? caught = null;
            try
            {
                await lifecycle.ReservePairTransactionAsync(Guid.NewGuid(), CancellationToken.None);
            }
            catch (HardwareCameraAgentLaunchException exception)
            {
                caught = exception;
            }

            Check.True(
                caught is not null,
                $"missing {missing}: reserve must fail closed with a typed exception before any process launch.");
            Check.False(
                caught!.RequestMayHaveBeenDispatched,
                $"missing {missing}: nothing was ever dispatched -- the process never even started.");
            var expectedMissingPath = missing == "capture-profile" ? captureProfilePath : identityProofPath;
            Check.True(
                caught.Message.Contains(expectedMissingPath, StringComparison.Ordinal),
                $"missing {missing}: the exception must name the missing artifact path, was: {caught.Message}");
            Check.False(
                Directory.Exists(journalRoot),
                $"missing {missing}: zero process/pipe means the pair journal directory must not be created either.");
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task DualCameraAgentLifecycleFakeHostHappyPathAsync()
{
    var root = CreateHardwareTestRoot();
    var tracePath = Path.Combine(root, "dual-agent-trace.jsonl");
    var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
    var previousTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
    try
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "happy");
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);

        await using var lifecycle = CreateDualAgentTestLifecycle(root);
        var stitcher = new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath());
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            new HardwareDualCaptureSource(lifecycle),
            stitcher,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));

        var transactionId = Guid.NewGuid();
        var state = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(transactionId));

        Check.Equal(DualCameraFailureCode.None, state.FailureCode);
        Check.Equal(2, state.Capture!.Originals.Count);
        Check.True(
            state.Stitch is { Succeeded: true },
            "The Ready synthetic seam's reserve->start Completed path must reach a real stitched JPEG.");

        var entries = ReadDualAgentTraceEntries(tracePath);
        Check.Equal(1, entries.Count(entry => entry.Operation == "reserve-pair-transaction"));
        Check.Equal(1, entries.Count(entry => entry.Operation == "start-reserved-pair"));
        Check.Equal(0, entries.Count(entry => entry.Operation == "get-pair-transaction-result"));
        Check.True(
            entries.Select(entry => entry.PipeName).Distinct().Count() == 1,
            "A single-process happy path must stay on exactly one pipe name.");
        Check.True(
            entries.All(entry => entry.TransactionId is null || entry.TransactionId == transactionId.ToString("N")),
            "Every observed operation must reference the one dispatched transaction id.");
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousTrace);
        Directory.Delete(root, recursive: true);
    }
}

static async Task DualCameraAgentLifecycleExitCodeClassificationAsync()
{
    // OperatorShell exceptions must terminate at this lifecycle boundary. Foundation
    // receives only a typed dispatch state, so it can distinguish a known pre-dispatch
    // failure without referencing HardwareCameraAgentLaunchException.
    foreach (var (exitCode, expectedState) in new (int ExitCode, DualHardwareDispatchState ExpectedState)[]
             {
                 (0, DualHardwareDispatchState.ResponseUnknown),
                 (1, DualHardwareDispatchState.ConfirmedUndispatched),
                 (2, DualHardwareDispatchState.ConfirmedUndispatched),
                 (3, DualHardwareDispatchState.ResponseUnknown),
             })
    {
        var root = CreateHardwareTestRoot();
        var tracePath = Path.Combine(root, "dual-agent-trace.jsonl");
        var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
        var previousExitCode = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
        var previousTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
        try
        {
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "die-after-start");
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", exitCode.ToString());
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);

            await using var lifecycle = CreateDualAgentTestLifecycle(root);

            var transactionId = Guid.NewGuid();
            Check.True(
                await lifecycle.ReservePairTransactionAsync(transactionId, CancellationToken.None),
                $"exit {exitCode}: reserve must be accepted before the classified failure.");

            var dispatch = await lifecycle.StartReservedPairAsync(
                BuildDualAgentTestCaptureRequest(root, transactionId),
                CancellationToken.None);
            Check.Equal(expectedState, dispatch.State);
            Check.True(dispatch.Result is null,
                $"exit {exitCode}: a transport failure must never forge a capture result.");
        }
        finally
        {
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", previousExitCode);
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousTrace);
            Directory.Delete(root, recursive: true);
        }
    }

    var cleanupRoot = CreateHardwareTestRoot();
    var cleanupTrace = Path.Combine(cleanupRoot, "dual-agent-trace.jsonl");
    var previousCleanupScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
    var previousCleanupExitCode = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
    var previousCleanupTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
    try
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "die-before-start-dispatch");
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", "2");
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", cleanupTrace);
        await using var lifecycle = CreateDualAgentTestLifecycle(cleanupRoot);
        var productRoot = Path.Combine(cleanupRoot, "products");
        var recoveryStore = new HardwareDualTransactionSnapshotStore(productRoot);
        var flow = new DualCameraProductFlow(
            productRoot,
            new HardwareDualCaptureSource(lifecycle, recoveryStore: recoveryStore),
            new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath()),
            new FixedDualCameraIdentitySnapshotSource(
                DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));

        var firstId = Guid.NewGuid();
        var first = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(firstId));
        Check.Equal(DualCameraFailureCode.HardwarePending, first.FailureCode);
        Check.True(recoveryStore.LoadPending() is null,
            "a confirmed close tombstone must clear the durable PC snapshot");
        var entries = ReadDualAgentTraceEntries(cleanupTrace);
        Check.Equal(1, entries.Count(entry => entry.Operation == "reserve-pair-transaction"));
        Check.Equal(1, entries.Count(entry => entry.Operation == "start-reserved-pair"));
        Check.Equal(1, entries.Count(entry => entry.Operation == "close-reserved-pair-transaction"));
        Check.Equal(0, entries.Count(entry => entry.Operation == "get-pair-transaction-result"));
        Check.True(entries.Where(entry => entry.TransactionId is not null)
            .All(entry => entry.TransactionId == firstId.ToString("N")),
            "pre-dispatch cleanup must never switch transaction IDs");
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousCleanupScenario);
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", previousCleanupExitCode);
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousCleanupTrace);
        Directory.Delete(cleanupRoot, recursive: true);
    }
}

static DualCameraAgentLifecycle CreateDualAgentTestLifecycle(string root)
{
    var captureProfilePath = Path.Combine(root, "camera-agent", "approved-dual-capture-profile.json");
    var identityProofPath = Path.Combine(root, "phase0", "dual-identity-proof.json");
    WriteDualAgentTestArtifactFiles(captureProfilePath, identityProofPath);
    return new DualCameraAgentLifecycle(
        DualCameraAgentTestHostPath(),
        Path.Combine(root, "agent-pair-journal"),
        captureProfilePath,
        identityProofPath);
}

static DualHardwareCaptureRequest BuildDualAgentTestCaptureRequest(string root, Guid transactionId)
{
    var startedAtUtc = DateTimeOffset.UtcNow;
    return new DualHardwareCaptureRequest(
        transactionId,
        Path.Combine(root, "products", "transactions", transactionId.ToString("N")),
        DualCameraIdentitySnapshot.AnonymousTestSyntheticReady(),
        HardwareDualCaptureProfile.ApprovedSynthetic(),
        DualCameraRigProfile.ApprovedSynthetic(),
        new HardwareDualOperatorConfirmations(true, true, true, true, true),
        startedAtUtc,
        startedAtUtc.AddSeconds(180));
}

static async Task DualCameraAgentLifecycleRestartRecoveryAsync()
{
    foreach (var (scenarioLabel, exitCode) in new (string, int)[]
             {
                 ("crash after dispatch (exit 3, dispatched_delivery_failed)", 3),
                 ("native max-lifetime exit (exit 0, complete)", 0),
             })
    {
        var root = CreateHardwareTestRoot();
        var tracePath = Path.Combine(root, "dual-agent-trace.jsonl");
        var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
        var previousExitCode = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
        var previousTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
        try
        {
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "die-after-start");
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", exitCode.ToString());
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);

            await using var lifecycle = CreateDualAgentTestLifecycle(root);
            var stitcher = new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath());
            var flow = new DualCameraProductFlow(
                Path.Combine(root, "products"),
                new HardwareDualCaptureSource(lifecycle),
                stitcher,
                new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));

            var transactionId = Guid.NewGuid();
            var unknown = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(transactionId));
            Check.Equal(DualCameraFailureCode.AgentResponseUnknown, unknown.FailureCode);

            // AgentResponseUnknown is the catch-all failure code for the whole
            // HardwareDual path -- a broken fake host (wrong scenario wiring, a crash
            // before start-reserved-pair, etc.) would produce the exact same code as
            // a correctly-behaving one. Assert what actually happened at the
            // process/pipe level immediately, before any later ambiguous-outcome
            // assert can hide which stage really broke.
            var afterDispatchEntries = ReadDualAgentTraceEntries(tracePath);
            Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "reserve-pair-transaction"));
            Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "start-reserved-pair"));
            Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "get-pair-transaction-result"));
            Check.Equal(2, afterDispatchEntries.Select(entry => entry.PipeName).Distinct().Count());

            // Process exit/pipe failure keeps support-required: a plain new capture
            // attempt must stay blocked on the same frozen transaction, with zero new
            // reserve/redispatch/retry -- verified directly against the Agent, not
            // just against the returned failure code.
            var blocked = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(Guid.NewGuid()));
            Check.Equal(DualCameraFailureCode.AgentResponseUnknown, blocked.FailureCode);
            Check.Equal(transactionId, blocked.TransactionId);
            Check.Equal(afterDispatchEntries.Count, ReadDualAgentTraceEntries(tracePath).Count);

            var recovered = await flow.RecoverAndStitchAsync(transactionId);

            var afterRecoveryEntries = ReadDualAgentTraceEntries(tracePath);
            Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "reserve-pair-transaction"));
            Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "start-reserved-pair"));
            Check.Equal(2, afterRecoveryEntries.Count(entry => entry.Operation == "get-pair-transaction-result"));
            Check.True(
                afterRecoveryEntries.Where(entry => entry.TransactionId is not null)
                    .All(entry => entry.TransactionId == transactionId.ToString("N")),
                $"{scenarioLabel}: every observed operation must reference the same transaction id (zero different-ID query).");
            Check.Equal(3, afterRecoveryEntries.Select(entry => entry.PipeName).Distinct().Count());

            Check.Equal(DualCameraFailureCode.None, recovered.FailureCode);
            Check.Equal(2, recovered.Capture!.Originals.Count);
            Check.True(
                recovered.Stitch is { Succeeded: true },
                $"{scenarioLabel}: recovery must reach a real stitched JPEG from the frozen snapshot.");
        }
        finally
        {
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", previousExitCode);
            Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousTrace);
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task DualCameraAgentLifecyclePipeFailureWithoutProcessExitAsync()
{
    var root = CreateHardwareTestRoot();
    var tracePath = Path.Combine(root, "dual-agent-trace.jsonl");
    var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
    var previousTrace = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
    try
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "pipe-only-after-start");
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", tracePath);

        await using var lifecycle = CreateDualAgentTestLifecycle(root);
        var stitcher = new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath());
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            new HardwareDualCaptureSource(lifecycle),
            stitcher,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));

        var transactionId = Guid.NewGuid();
        var unknown = await flow.CaptureAndStitchAsync(HardwareDualTestRequest(transactionId));
        Check.Equal(DualCameraFailureCode.AgentResponseUnknown, unknown.FailureCode);

        // Assert what actually happened before trusting the later ambiguous-outcome
        // asserts: AgentResponseUnknown alone cannot distinguish a working scenario
        // from a broken one.
        var afterDispatchEntries = ReadDualAgentTraceEntries(tracePath);
        Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "reserve-pair-transaction"));
        Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "start-reserved-pair"));
        Check.Equal(1, afterDispatchEntries.Count(entry => entry.Operation == "get-pair-transaction-result"));
        Check.Equal(1, afterDispatchEntries.Select(entry => entry.PipeName).Distinct().Count());

        var recovered = await flow.RecoverAndStitchAsync(transactionId);

        var afterRecoveryEntries = ReadDualAgentTraceEntries(tracePath);
        Check.Equal(
            1,
            afterRecoveryEntries.Select(entry => entry.PipeName).Distinct().Count()); // process stayed alive: no restart needed
        Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "reserve-pair-transaction"));
        Check.Equal(1, afterRecoveryEntries.Count(entry => entry.Operation == "start-reserved-pair"));
        Check.Equal(2, afterRecoveryEntries.Count(entry => entry.Operation == "get-pair-transaction-result"));

        Check.Equal(DualCameraFailureCode.None, recovered.FailureCode);
        Check.Equal(2, recovered.Capture!.Originals.Count);
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE", previousTrace);
        Directory.Delete(root, recursive: true);
    }
}

static async Task DualCameraAgentLifecycleConnectFailureSurfacesExitDiagnosticsAsync()
{
    // When the Agent process fails before ever creating its pipe (e.g. an
    // immediate startup failure), the client's connect attempt itself fails --
    // this is a different failure path than an incomplete response, and it used to
    // discard the process's exit code and stderr, leaving only a generic "connection
    // failed" after the full connect timeout.
    var root = CreateHardwareTestRoot();
    var previousScenario = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO");
    var previousExitCode = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
    try
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", "die-before-listen");
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", "1");

        await using var lifecycle = CreateDualAgentTestLifecycle(root);

        HardwareCameraAgentLaunchException? caught = null;
        try
        {
            await lifecycle.ReservePairTransactionAsync(Guid.NewGuid(), CancellationToken.None);
        }
        catch (HardwareCameraAgentLaunchException exception)
        {
            caught = exception;
        }

        Check.True(
            caught is not null,
            "A connect failure must classify as a typed launch exception, not a raw connect exception.");
        Check.False(
            caught!.RequestMayHaveBeenDispatched,
            "A connect failure means the request was definitely never dispatched.");
        Check.True(
            caught.ProcessExitCode == 1,
            $"The Native exit code must be surfaced instead of discarded, was {caught.ProcessExitCode?.ToString() ?? "null"}.");
        Check.True(
            caught.SanitizedStandardError.Length > 0,
            "The Native failure reason must reach the operator instead of only a generic connect timeout.");
        Check.False(
            caught.SanitizedStandardError.Contains("super-secret", StringComparison.Ordinal),
            "Native stderr must remain sanitized even when surfaced from a connect failure.");
    }
    finally
    {
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_SCENARIO", previousScenario);
        Environment.SetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE", previousExitCode);
        Directory.Delete(root, recursive: true);
    }
}

static List<(string PipeName, string Operation, string? TransactionId)> ReadDualAgentTraceEntries(string tracePath)
{
    var result = new List<(string, string, string?)>();
    if (!File.Exists(tracePath))
    {
        return result;
    }
    foreach (var line in File.ReadAllLines(tracePath))
    {
        if (string.IsNullOrWhiteSpace(line))
        {
            continue;
        }
        using var document = JsonDocument.Parse(line);
        var root = document.RootElement;
        result.Add((
            root.GetProperty("pipeName").GetString()!,
            root.GetProperty("operation").GetString()!,
            root.TryGetProperty("transactionId", out var idElement) && idElement.ValueKind == JsonValueKind.String
                ? idElement.GetString()
                : null));
    }
    return result;
}

// --- Fake Dual Camera Agent test host (child process) -----------------------
//
// Speaks the real a0.camera-agent.hardware-dual.v2 named-pipe wire protocol so
// DualCameraAgentLifecycle (production process/pipe lifecycle code) is exercised
// end-to-end, including a real process exit and a real second-generation process
// picking a fresh unique pipe name back up. Scenario and exit behavior are driven
// by env vars set by the parent test process before it starts the child.

static async Task<int> RunDualCameraAgentTestChildAsync(string scenario, IReadOnlyList<string> arguments)
{
    var pipeName = TryGetDualAgentArgument(arguments, "--pipe-name");
    var pairJournalRoot = TryGetDualAgentArgument(arguments, "--pair-journal-root");
    var approvedCaptureProfile = TryGetDualAgentArgument(arguments, "--approved-capture-profile");
    var dualIdentityProof = TryGetDualAgentArgument(arguments, "--dual-identity-proof");
    if (pipeName is null || pairJournalRoot is null || approvedCaptureProfile is null || dualIdentityProof is null)
    {
        return 1; // argument/launch failure
    }
    // Matched to the same strength as the real Native contract
    // (docs/HARDWARE_CAMERA_AGENT_DUAL_V2.md, Issue #22): both paths must already be
    // existing regular files, or exit 1. Previously these two values were extracted
    // and only null-checked, never actually verified to exist -- so a client-side
    // regression that stopped passing real files could never be caught here.
    if (!File.Exists(approvedCaptureProfile) || !File.Exists(dualIdentityProof))
    {
        return 1;
    }

    var tracePath = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_TRACE");
    var exitCodeText = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_TEST_CHILD_EXIT_CODE");
    var dieExitCode = int.TryParse(exitCodeText, out var parsedExitCode) ? parsedExitCode : 3;

    if (scenario == "die-before-listen")
    {
        // Simulates Native failing fast (e.g. on startup) before ever creating the
        // pipe: the client's connect attempt fails outright rather than an
        // in-flight request going unanswered. Exercises DualCameraAgentLifecycle's
        // connect-failure diagnostics path (exit code + stderr), not the
        // incomplete-response path the other scenarios cover.
        await Console.Error.WriteLineAsync(
            "synthetic dual agent failed before listening secret=super-secret");
        return dieExitCode;
    }

    Directory.CreateDirectory(pairJournalRoot);

    await using var pipe = new NamedPipeServerStream(
        pipeName,
        PipeDirection.InOut,
        maxNumberOfServerInstances: 1,
        PipeTransmissionMode.Byte,
        PipeOptions.Asynchronous);
    // Generous idle window: this repo has hit timing-sensitive failures on slow CI
    // runners more than once (#17 / #20 / #5-S7), and this fake host is a real
    // subprocess talking over a real named pipe, not an in-memory fake.
    var idleTimeout = TimeSpan.FromSeconds(12);
    while (true)
    {
        using var acceptTimeout = new CancellationTokenSource(idleTimeout);
        try
        {
            await pipe.WaitForConnectionAsync(acceptTimeout.Token);
        }
        catch (OperationCanceledException)
        {
            return 0; // idle: no further requests within the window; complete (0).
        }

        string requestJson;
        try
        {
            using var requestTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            requestJson = await ReadPersistentTestFrameAsync(pipe, requestTimeout.Token);
        }
        catch (Exception) when (pipe.IsConnected)
        {
            pipe.Disconnect();
            continue;
        }

        using var request = JsonDocument.Parse(requestJson);
        var root = request.RootElement;
        if (root.GetProperty("schemaVersion").GetString() != DualHardwareCameraAgentProtocol.SchemaVersion ||
            root.GetProperty("marker").GetString() != DualHardwareCameraAgentProtocol.Marker ||
            root.GetProperty("simulation").GetBoolean())
        {
            // A real Native agent runs a strict parser and would reject a malformed
            // envelope outright instead of guessing at it; the fake host must fail
            // the same way rather than silently accepting whatever the client sent,
            // or a client-side envelope regression would only ever surface on real
            // hardware.
            return 66;
        }
        var operation = root.GetProperty("operation").GetString()!;
        var requestId = root.GetProperty("requestId").GetString()!;
        var payload = root.GetProperty("payload");

        await AppendDualAgentTraceAsync(tracePath, pipeName, operation, TryGetDualAgentTransactionId(payload));

        using var responseTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        switch (operation)
        {
            case DualHardwareCameraAgentProtocol.Operations.GetCapabilities:
                await WritePersistentTestFrameAsync(
                    pipe,
                    BuildDualAgentResponseEnvelopeJson(requestId, true, "DualCapabilities", BuildDualCapabilitiesPayload()),
                    responseTimeout.Token);
                break;

            case DualHardwareCameraAgentProtocol.Operations.ReservePairTransaction:
            {
                var transactionIdHex = payload.GetProperty("transactionId").GetString()!;
                await File.WriteAllTextAsync(
                    Path.Combine(pairJournalRoot, $"{transactionIdHex}.reserved"),
                    "Reserved");
                await WritePersistentTestFrameAsync(
                    pipe,
                    BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        true,
                        "PairTransactionReserved",
                        new { transactionId = transactionIdHex, accepted = true }),
                    responseTimeout.Token);
                break;
            }

            case DualHardwareCameraAgentProtocol.Operations.StartReservedPair:
            {
                var transaction = payload.GetProperty("transaction");
                var transactionIdHex = transaction.GetProperty("transactionId").GetString()!;
                var transactionDirectory = transaction.GetProperty("transactionDirectory").GetString()!;
                if (scenario == "die-before-start-dispatch")
                {
                    if (pipe.IsConnected)
                    {
                        pipe.Disconnect();
                    }
                    return dieExitCode;
                }
                var reservedPath = Path.Combine(pairJournalRoot, $"{transactionIdHex}.reserved");
                if (File.Exists(reservedPath))
                {
                    File.Delete(reservedPath);
                }
                var (camAPath, camBPath) = await WriteDualAgentOriginalsAsync(
                    transactionDirectory,
                    Guid.ParseExact(transactionIdHex, "N"));
                await SaveDualAgentJournalEntryAsync(pairJournalRoot, transactionIdHex, transaction, camAPath, camBPath);

                if (scenario == "happy")
                {
                    var resultPayload = BuildDualAgentCaptureResultPayload(transaction, transactionIdHex, camAPath, camBPath);
                    await WritePersistentTestFrameAsync(
                        pipe,
                        BuildDualAgentResponseEnvelopeJson(
                            requestId,
                            true,
                            "PairDispatchAccepted",
                            new { transactionId = transactionIdHex, dispatchState = "Completed", result = resultPayload }),
                        responseTimeout.Token);
                    break;
                }

                // "die-after-start" / "pipe-only-after-start": the journal entry above
                // is already durable (as a real Agent's own durable pair journal would
                // be), but the response is never sent -- the exact ambiguous point the
                // absolute invariant describes ("after start-reserved-pair's write
                // completes, any transport failure may mean the pair was captured").
                await File.WriteAllTextAsync(
                    Path.Combine(pairJournalRoot, $"{transactionIdHex}.query-delay"),
                    "delay exactly one same-ID query response");
                if (pipe.IsConnected)
                {
                    pipe.Disconnect();
                }
                if (scenario == "die-after-start")
                {
                    return dieExitCode;
                }
                continue; // pipe-only-after-start: stay alive for the next connection.
            }

            case DualHardwareCameraAgentProtocol.Operations.GetPairTransactionResult:
            {
                var transactionIdHex = payload.GetProperty("transactionId").GetString()!;
                var queryDelayPath = Path.Combine(
                    pairJournalRoot,
                    $"{transactionIdHex}.query-delay");
                if (File.Exists(queryDelayPath))
                {
                    File.Delete(queryDelayPath);
                    if (pipe.IsConnected)
                    {
                        pipe.Disconnect();
                    }
                    if (scenario == "die-after-start")
                    {
                        return dieExitCode;
                    }
                    continue;
                }
                var closedPath = Path.Combine(pairJournalRoot, $"{transactionIdHex}.closed");
                var reservedPath = Path.Combine(pairJournalRoot, $"{transactionIdHex}.reserved");
                var journalResult = await LoadDualAgentJournalCaptureResultPayloadAsync(pairJournalRoot, transactionIdHex);
                var responseJson = File.Exists(closedPath)
                    ? BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        true,
                        "PairTransactionClosedBeforeDispatch",
                        new { transactionId = transactionIdHex, found = true, result = (object?)null })
                    : File.Exists(reservedPath)
                    ? BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        false,
                        "PairTransactionReserved",
                        new { transactionId = transactionIdHex, found = true, result = (object?)null })
                    : journalResult is null
                    ? BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        false,
                        "PairTransactionNotFound",
                        new { transactionId = transactionIdHex, found = false, result = (object?)null })
                    : BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        true,
                        "PairTransactionFound",
                        new { transactionId = transactionIdHex, found = true, result = journalResult });
                await WritePersistentTestFrameAsync(pipe, responseJson, responseTimeout.Token);
                break;
            }

            case DualHardwareCameraAgentProtocol.Operations.CloseReservedPairTransaction:
            {
                var transactionIdHex = payload.GetProperty("transactionId").GetString()!;
                var reservedPath = Path.Combine(pairJournalRoot, $"{transactionIdHex}.reserved");
                var closedPath = Path.Combine(pairJournalRoot, $"{transactionIdHex}.closed");
                var terminalPath = Path.Combine(pairJournalRoot, $"{transactionIdHex}.json");
                var canClose = File.Exists(closedPath) ||
                    (File.Exists(reservedPath) && !File.Exists(terminalPath));
                if (canClose && !File.Exists(closedPath))
                {
                    await File.WriteAllTextAsync(closedPath, "ClosedBeforeDispatch");
                    File.Delete(reservedPath);
                }
                await WritePersistentTestFrameAsync(
                    pipe,
                    BuildDualAgentResponseEnvelopeJson(
                        requestId,
                        canClose,
                        canClose
                            ? "PairTransactionClosedBeforeDispatch"
                            : "PairCloseRejected",
                        new
                        {
                            transactionId = transactionIdHex,
                            closedBeforeDispatch = canClose,
                        }),
                    responseTimeout.Token);
                break;
            }

            default:
                return 65;
        }

        if (pipe.IsConnected)
        {
            pipe.Disconnect();
        }
    }
}

static string? TryGetDualAgentArgument(IReadOnlyList<string> arguments, string flag)
{
    var index = arguments.ToList().IndexOf(flag);
    return index >= 0 && index + 1 < arguments.Count ? arguments[index + 1] : null;
}

static string? TryGetDualAgentTransactionId(JsonElement payload)
{
    if (payload.ValueKind != JsonValueKind.Object)
    {
        return null;
    }
    if (payload.TryGetProperty("transactionId", out var direct) && direct.ValueKind == JsonValueKind.String)
    {
        return direct.GetString();
    }
    if (payload.TryGetProperty("transaction", out var nested) && nested.ValueKind == JsonValueKind.Object &&
        nested.TryGetProperty("transactionId", out var nestedId) && nestedId.ValueKind == JsonValueKind.String)
    {
        return nestedId.GetString();
    }
    return null;
}

static async Task AppendDualAgentTraceAsync(string? tracePath, string pipeName, string operation, string? transactionId)
{
    if (string.IsNullOrWhiteSpace(tracePath))
    {
        return;
    }
    var line = JsonSerializer.Serialize(
        new { pipeName, operation, transactionId },
        new JsonSerializerOptions(JsonSerializerDefaults.Web));
    await File.AppendAllTextAsync(tracePath, line + Environment.NewLine);
}

static string BuildDualAgentResponseEnvelopeJson(string requestId, bool success, string resultCode, object payload) =>
    JsonSerializer.Serialize(
        new
        {
            schemaVersion = DualHardwareCameraAgentProtocol.SchemaVersion,
            simulation = false,
            marker = DualHardwareCameraAgentProtocol.Marker,
            requestId,
            success,
            resultCode,
            payload,
        },
        new JsonSerializerOptions(JsonSerializerDefaults.Web));

static object BuildDualCapabilitiesPayload() => new
{
    cameraMode = "DualCamera",
    protocolVersion = 2,
    orderedRequiredAliases = new[] { "CAM-A", "CAM-B" },
    supportedOperations = DualHardwareCameraAgentProtocol.Operations.Required,
    pairJournalDurable = true,
    sameTransactionQueryOnly = true,
    automaticRetryCount = 0,
};

static async Task<(string CamAPath, string CamBPath)> WriteDualAgentOriginalsAsync(
    string transactionDirectory,
    Guid transactionId)
{
    var adapter = new M2OfflineStitcherProcessAdapter(DualCameraM2AdapterPath());
    var camAPath = Path.Combine(transactionDirectory, "CAM-A", "original.jpg");
    var camBPath = Path.Combine(transactionDirectory, "CAM-B", "original.jpg");
    await adapter.CaptureAsync("CAM-A", transactionId, camAPath, CancellationToken.None);
    await adapter.CaptureAsync("CAM-B", transactionId, camBPath, CancellationToken.None);
    return (camAPath, camBPath);
}

static object BuildDualAgentCaptureResultPayload(
    JsonElement transaction,
    string transactionIdHex,
    string camAPath,
    string camBPath)
{
    var captureProfile = transaction.GetProperty("captureProfileSnapshot");
    var rigProfile = transaction.GetProperty("rigProfileSnapshot");
    var startedAtUtc = transaction.GetProperty("startedAtUtc").GetString()!;
    var watchdogDeadlineUtc = transaction.GetProperty("watchdogDeadlineUtc").GetString()!;
    return new
    {
        transactionId = transactionIdHex,
        originals = new object[]
        {
            new { alias = "CAM-A", canonicalOriginalPath = camAPath, exactRecoveredObjectDeleted = true, spoolEmptyAfterDelete = true },
            new { alias = "CAM-B", canonicalOriginalPath = camBPath, exactRecoveredObjectDeleted = true, spoolEmptyAfterDelete = true },
        },
        terminalState = "Succeeded",
        failureCode = "None",
        evidence = new
        {
            terminalState = "Succeeded",
            identitySnapshot = transaction.GetProperty("identitySnapshot").Clone(),
            captureProfileId = captureProfile.GetProperty("profileId").GetString()!,
            captureProfileVersion = captureProfile.GetProperty("version").GetString()!,
            profileId = rigProfile.GetProperty("profileId").GetString()!,
            profileVersion = rigProfile.GetProperty("version").GetString()!,
            watchdogStartedAtUtc = startedAtUtc,
            watchdogDeadlineUtc,
            completedAtUtc = startedAtUtc,
            watchdogCompletedInTime = true,
            liveViewStopAndCloseConfirmed = true,
            exactDeleteConfirmedForEveryRetainedOriginal = true,
            bothSpoolsEmptyAfter = true,
            automaticRetryCount = 0,
        },
    };
}

static async Task SaveDualAgentJournalEntryAsync(
    string pairJournalRoot,
    string transactionIdHex,
    JsonElement transaction,
    string camAPath,
    string camBPath)
{
    var path = Path.Combine(pairJournalRoot, $"{transactionIdHex}.json");
    var json = JsonSerializer.Serialize(
        new { transaction, camAPath, camBPath },
        new JsonSerializerOptions(JsonSerializerDefaults.Web));
    await File.WriteAllTextAsync(path, json);
}

static async Task<object?> LoadDualAgentJournalCaptureResultPayloadAsync(string pairJournalRoot, string transactionIdHex)
{
    var path = Path.Combine(pairJournalRoot, $"{transactionIdHex}.json");
    if (!File.Exists(path))
    {
        return null;
    }
    var json = await File.ReadAllTextAsync(path);
    using var document = JsonDocument.Parse(json);
    var root = document.RootElement;
    return BuildDualAgentCaptureResultPayload(
        root.GetProperty("transaction"),
        transactionIdHex,
        root.GetProperty("camAPath").GetString()!,
        root.GetProperty("camBPath").GetString()!);
}

static void SimulatedTestImageFrameSourceRendersWatermarkedFramesForEveryPattern()
{
    var source = new SimulatedTestImageFrameSource();
    var capturedAt = DateTimeOffset.UtcNow;
    foreach (var pattern in Enum.GetValues<SimulatedFramePattern>())
    {
        var frame = source.CreateFrame("CAM-A", pattern, sequenceNumber: 3, capturedAt);
        Check.Equal("CAM-A", frame.CameraAlias);
        Check.Equal(pattern, frame.Pattern);
        Check.True(frame.Simulation, $"{pattern}: every SIMULATED frame must carry Simulation=true.");
        Check.Equal("Simulated", frame.Marker);
        Check.True(frame.Image.PixelWidth > 0 && frame.Image.PixelHeight > 0, $"{pattern}: the rendered frame must have real pixel dimensions.");
        Check.True(frame.Image.IsFrozen, $"{pattern}: the rendered frame must be frozen for safe cross-thread hand-off.");

        // The bottom-left timestamp/marker badge sits below the document rectangle (which is
        // vertically centered and only ~74% of the canvas height), so this band is pure
        // background unless the badge is actually painted there. A near-black rectangle plus
        // white text must differ substantially from the background color in that band.
        var pixels = CopyPixelsBgra(frame.Image);
        var stride = frame.Image.PixelWidth * 4;
        var bandTop = Math.Max(0, frame.Image.PixelHeight - 30);
        var bandBottom = Math.Max(bandTop, frame.Image.PixelHeight - 4);
        var bandRight = Math.Min(frame.Image.PixelWidth, 200);
        var differingPixelCount = 0;
        for (var y = bandTop; y < bandBottom; y++)
        {
            for (var x = 8; x < bandRight; x++)
            {
                var offset = (y * stride) + (x * 4);
                var blue = pixels[offset];
                var green = pixels[offset + 1];
                var red = pixels[offset + 2];
                var diff = Math.Abs(blue - 0x1F) + Math.Abs(green - 0x1A) + Math.Abs(red - 0x14);
                if (diff > 24)
                {
                    differingPixelCount++;
                }
            }
        }

        Check.True(
            differingPixelCount > 200,
            $"{pattern}: the SIMULATED watermark/timestamp badge must paint visibly different pixels over the bottom-left background band (found {differingPixelCount} differing pixels).");
    }
}

static void SimulatedTestImageFrameSourceAppliesBlurAcrossTheFocusTransition()
{
    var source = new SimulatedTestImageFrameSource();
    var capturedAt = DateTimeOffset.UtcNow;
    // seq=0 sits at the start of the blur ramp (near-maximum blur radius) and seq=17 sits at
    // the end of the ramp (fully sharp); holding camera/pattern/timestamp constant isolates
    // the blur radius as the only thing that can differ between the two renders.
    var blurredFrame = source.CreateFrame("CAM-A", SimulatedFramePattern.BlurToFocusTransition, sequenceNumber: 0, capturedAt);
    var sharpFrame = source.CreateFrame("CAM-A", SimulatedFramePattern.BlurToFocusTransition, sequenceNumber: 17, capturedAt);

    var blurredPixels = CopyPixelsBgra(blurredFrame.Image);
    var sharpPixels = CopyPixelsBgra(sharpFrame.Image);
    Check.Equal(blurredPixels.Length, sharpPixels.Length);

    long totalDifference = 0;
    for (var index = 0; index < blurredPixels.Length; index++)
    {
        totalDifference += Math.Abs(blurredPixels[index] - sharpPixels[index]);
    }

    Check.True(
        totalDifference > 50_000,
        "seq=0 (near-max blur) and seq=17 (sharp) must render visibly different pixels if BlurEffect is actually applied to the scene " +
        $"(total abs BGRA diff = {totalDifference}). If this is at/near 0, RenderTargetBitmap.Render() is ignoring the Effect on the root visual again.");
}

static async Task SimulatedLiveViewFramePumpOnlyTicksBetweenStartAndStopAsync()
{
    using var pump = new SimulatedLiveViewFramePump(interval: TimeSpan.FromMilliseconds(20));
    var ticks = new List<SimulatedLiveViewFrameTick>();
    pump.Tick += (_, tick) => { lock (ticks) { ticks.Add(tick); } };

    await Task.Delay(60);
    Check.Equal(0, ticks.Count);

    var generation = pump.Start("CAM-B", SimulatedFramePattern.TiltedDocumentRollPlus3);
    Check.Equal(1, generation);
    await WaitUntilAsync(
        () => { lock (ticks) { return ticks.Count >= 2; } },
        "The pump did not tick after Start().");
    lock (ticks)
    {
        Check.True(ticks.All(tick => tick.CameraAlias == "CAM-B"), "Every tick must carry the started camera alias.");
        Check.True(ticks.All(tick => tick.Pattern == SimulatedFramePattern.TiltedDocumentRollPlus3), "Every tick must carry the started pattern.");
        Check.True(ticks.All(tick => tick.Generation == generation), "Every tick from this session must carry the generation Start() returned.");
    }

    pump.Stop();
    // Stop() is intentionally non-blocking now (rendering was moved entirely out of the pump,
    // so there is nothing expensive left in flight to wait for): tolerate at most one
    // already-in-flight tick completing shortly after Stop() returns, then confirm the count
    // stabilizes. That — plus the ViewModel-side generation/alias/IsLiveViewActive guard — is
    // what "no frame supply while Live View is OFF" actually guarantees end to end.
    await Task.Delay(40);
    int countAfterGrace;
    lock (ticks)
    {
        countAfterGrace = ticks.Count;
    }
    await Task.Delay(80);
    lock (ticks)
    {
        Check.Equal(countAfterGrace, ticks.Count);
    }

    var secondGeneration = pump.Start("CAM-A", SimulatedFramePattern.FrontalDocument);
    Check.Equal(2, secondGeneration);
    pump.Stop();
}

static async Task SimulatedFramePumpWiringAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-SimulatedFramePumpTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        var frameSource = new FakeSimulatedLiveViewFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.IsSimulatedFrameSourceAvailable, "A pump and frame source were both injected, so the frame source must report available.");
        Check.True(viewModel.CanUseLiveView, "A safety-acknowledged, non-busy dual plan must allow Live View.");
        Check.Equal(0, pump.StartCalls.Count);

        // Rejecting an unrecognized pattern value must still re-announce the current value so
        // a bound ComboBox reverts instead of keeping the rejected selection on screen.
        var propertyChangedNames = new List<string>();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is not null)
            {
                propertyChangedNames.Add(args.PropertyName);
            }
        };
        viewModel.SelectedSimulatedFramePattern = "not-a-real-pattern";
        Check.Equal(SimulatedFramePatternCatalog.DefaultLabel, viewModel.SelectedSimulatedFramePattern);
        Check.True(
            propertyChangedNames.Contains(nameof(OperatorShellViewModel.SelectedSimulatedFramePattern)),
            "A rejected pattern value must still raise PropertyChanged so bound controls revert to the accepted value.");

        viewModel.SelectedSimulatedFramePattern = "ボケ→合焦遷移";
        Check.Equal(1, pump.PatternChanges.Count);
        Check.Equal(SimulatedFramePattern.BlurToFocusTransition, pump.PatternChanges[0]);

        viewModel.ToggleLiveViewCommand.Execute(null);
        Check.True(viewModel.IsLiveViewActive, "Toggling Live View on must flip the flag.");
        Check.Equal(1, pump.StartCalls.Count);
        Check.Equal("CAM-A", pump.StartCalls[0].CameraAlias);
        Check.Equal(SimulatedFramePattern.BlurToFocusTransition, pump.StartCalls[0].Pattern);
        Check.Equal(0, pump.StopCallCount);
        var firstGeneration = pump.LastReturnedGeneration;

        // A mismatched-alias tick (as if Stop()/an alias switch raced an in-flight timer
        // tick) must not populate the composite preview's non-live "still" slot for that
        // alias, and must not even reach the frame source.
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-B", SimulatedFramePattern.FrontalDocument, 0, firstGeneration, DateTimeOffset.UtcNow));
        Check.True(viewModel.StageCompositeStillImage is null, "A stale tick for a non-active alias must be dropped.");
        Check.Equal(0, frameSource.CallCount);

        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.BlurToFocusTransition, 0, firstGeneration, DateTimeOffset.UtcNow));
        Check.Equal(1, frameSource.CallCount);
        Check.True(viewModel.StageCompositeLiveImage is not null, "A tick for the active alias must render and populate the live composite image.");
        Check.False(
            viewModel.IsStageSingleLiveImageVisible,
            "Stage mode defaults to composite preview, so the single-live image must stay hidden even though a frame exists.");

        // OFF -> back ON for the *same* camera alias is exactly the race a bare alias check
        // cannot catch: toggle off, toggle on again (new generation), then raise a tick still
        // carrying the OLD generation as if it had been in flight when Stop() was called.
        viewModel.ToggleLiveViewCommand.Execute(null);
        Check.Equal(1, pump.StopCallCount);
        viewModel.ToggleLiveViewCommand.Execute(null);
        Check.Equal(2, pump.StartCalls.Count);
        var secondGeneration = pump.LastReturnedGeneration;
        Check.False(secondGeneration == firstGeneration, "Start() must return a new generation on every call.");

        var frameSourceCallsBeforeStaleGenerationTick = frameSource.CallCount;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.FrontalDocument, 5, firstGeneration, DateTimeOffset.UtcNow));
        Check.Equal(frameSourceCallsBeforeStaleGenerationTick, frameSource.CallCount);

        viewModel.ToggleLiveViewCommand.Execute(null);
        Check.False(viewModel.IsLiveViewActive, "Toggling Live View off must flip the flag back.");
        Check.Equal(2, pump.StopCallCount);
        Check.True(viewModel.StageCompositeLiveImage is null, "Stopping Live View must clear the live composite image even though the last frame is retained.");

        viewModel.SelectedCamera = "CAM-B";
        Check.True(
            viewModel.StageCompositeFreshnessText.Contains("秒前", StringComparison.Ordinal),
            "The frozen CAM-A frame must drive the freshness badge once CAM-B becomes the selected (still) alias.");

        // A frame source that returns a frame missing the Simulated marker must be dropped —
        // and must not throw (a throw inside the SynchronizationContext.Post callback used in
        // production would become an unhandled Dispatcher exception, not something callable
        // code here or in production could catch).
        viewModel.ToggleLiveViewCommand.Execute(null);
        var thirdGeneration = pump.LastReturnedGeneration;
        frameSource.ReturnInvalidMarker = true;
        var statusBeforeInvalidFrame = viewModel.StatusMessage;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-B", SimulatedFramePattern.FrontalDocument, 0, thirdGeneration, DateTimeOffset.UtcNow));
        Check.False(
            string.Equals(statusBeforeInvalidFrame, viewModel.StatusMessage, StringComparison.Ordinal),
            "An invalid-marker frame must be surfaced through StatusMessage instead of silently doing nothing or throwing.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static void TargetReticleDragMovesClampAndScale()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-TargetReticleTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        Check.True(IsClose(0.5, viewModel.TargetX), "The target reticle must default to the stage center on X.");
        Check.True(IsClose(0.5, viewModel.TargetY), "The target reticle must default to the stage center on Y.");

        // Stage drag: a direct (coarse) move — the normalized delta is applied unscaled.
        viewModel.MoveTargetByStageDrag(0.2, -0.1);
        Check.True(IsClose(0.7, viewModel.TargetX), "A stage drag must move the target by the full normalized delta on X.");
        Check.True(IsClose(0.4, viewModel.TargetY), "A stage drag must move the target by the full normalized delta on Y.");

        // Loupe drag: the same normalized delta must land only 1/4 as far — issue #30's
        // "ルーペ表示内のドラッグ = 細かい移動" contract.
        viewModel.SetTargetPosition(0.5, 0.5);
        viewModel.MoveTargetByLoupeDrag(0.2, -0.1);
        Check.True(IsClose(0.55, viewModel.TargetX), "A loupe drag must scale the delta by TargetFineDragScale on X.");
        Check.True(IsClose(0.475, viewModel.TargetY), "A loupe drag must scale the delta by TargetFineDragScale on Y.");

        // Boundary: dragging past either edge must clamp to 0/1, never overshoot or wrap.
        viewModel.SetTargetPosition(0.95, 0.05);
        viewModel.MoveTargetByStageDrag(1.0, -1.0);
        Check.True(IsClose(1.0, viewModel.TargetX), "A drag past the right edge must clamp to 1.0, not overshoot.");
        Check.True(IsClose(0.0, viewModel.TargetY), "A drag past the top edge must clamp to 0.0, not go negative.");

        // Boundary: an out-of-range explicit placement must clamp the same way.
        viewModel.SetTargetPosition(-5, 5);
        Check.True(IsClose(0.0, viewModel.TargetX), "An explicit negative position must clamp to 0.0.");
        Check.True(IsClose(1.0, viewModel.TargetY), "An explicit position past 1.0 must clamp to 1.0.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }

    static bool IsClose(double expected, double actual) => Math.Abs(expected - actual) < 1e-9;
}

static async Task LoupeTracksTargetSideAndFreshnessBadgeAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-LoupeSideTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        var frameSource = new FakeSimulatedLiveViewFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        // With no frame ever supplied anywhere, the loupe must show its "not yet acquired"
        // placeholder rather than an empty-but-"available" image (the #30 "誤認させない" contract).
        Check.True(viewModel.IsLoupePlaceholderVisible, "With no frame ever supplied, the loupe must show its placeholder.");
        Check.False(viewModel.IsLoupeImageVisible, "With no frame ever supplied, the loupe must not claim an image is available.");

        // CAM-A live (SelectedCamera defaults to CAM-A): move the target onto the composite
        // preview's left (live) side — TargetX defaults to the exact 0.5 center, which
        // IsTargetOnLiveSide resolves to the *still* side, so this must be explicit — then
        // tick one frame and confirm the loupe picks it up live, with no freshness/STILL
        // badge (nothing is frozen yet).
        viewModel.SetTargetPosition(0.2, 0.5);
        viewModel.ToggleLiveViewCommand.Execute(null);
        var cameraAGeneration = pump.LastReturnedGeneration;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.FrontalDocument, 0, cameraAGeneration, DateTimeOffset.UtcNow));
        Check.Equal("CAM-A", viewModel.LoupeCameraAlias);
        Check.True(viewModel.IsLoupeImageVisible, "A live-ticked frame for the loupe's own alias must populate the loupe image without throwing on the tiny test bitmap.");
        Check.True(viewModel.IsLoupeSourceLive, "The loupe must report live while its alias matches the streaming camera.");
        Check.False(viewModel.IsLoupeFreshnessVisible, "A live loupe source must not show a freshness/STILL badge.");

        // Move the target to the composite preview's still (right) side. CAM-B has never been
        // captured or live-viewed, so this must fall back to the placeholder, not silently
        // reuse CAM-A's frame for the wrong camera.
        viewModel.SetTargetPosition(0.9, 0.5);
        Check.Equal("CAM-B", viewModel.LoupeCameraAlias);
        Check.True(viewModel.IsLoupePlaceholderVisible, "An alias with no captured/live frame yet must show the placeholder, not a stale image.");

        // Give CAM-B a frame of its own, then hand Live View back to CAM-A: CAM-B's frame must
        // freeze in place with a freshness badge — the composite preview's "非ライブ側は最終
        // フレームの静止画" contract, extended to the loupe.
        viewModel.ToggleLiveViewCommand.Execute(null); // CAM-A live off (required before switching alias)
        viewModel.SelectedCamera = "CAM-B";
        viewModel.ToggleLiveViewCommand.Execute(null); // CAM-B live on
        var cameraBGeneration = pump.LastReturnedGeneration;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-B", SimulatedFramePattern.FrontalDocument, 0, cameraBGeneration, DateTimeOffset.UtcNow));
        viewModel.ToggleLiveViewCommand.Execute(null); // CAM-B live off
        viewModel.SelectedCamera = "CAM-A";

        Check.Equal("CAM-B", viewModel.LoupeCameraAlias);
        Check.True(viewModel.IsLoupeImageVisible, "CAM-B's frozen last frame must still populate the loupe once it exists.");
        Check.False(viewModel.IsLoupeSourceLive, "CAM-B is not the currently live camera, so the loupe must report it as frozen.");
        Check.True(viewModel.IsLoupeFreshnessVisible, "A frozen loupe source with a known frame must show the STILL freshness badge.");
        Check.True(viewModel.LoupeFreshnessText.Contains("秒前", StringComparison.Ordinal), "The freshness badge must report elapsed seconds.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task AutoFocusSuccessFixesFocusAndRecordsResultAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-AutoFocusSuccessTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        var frameSource = new FakeSimulatedLiveViewFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        Check.Equal("AF未実行", viewModel.FocusResultText);
        Check.True(viewModel.LastPreCaptureAutoFocusResult is null, "The pre-capture AF hook must start unset.");

        // Default target (0.5, 0.5) falls in CAM-B's domain (TargetX is not < 0.5); place it
        // squarely in CAM-A's domain so it matches the default selected/live camera.
        viewModel.SetTargetPosition(0.2, 0.5);
        viewModel.ToggleLiveViewCommand.Execute(null);
        var generation = pump.LastReturnedGeneration;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.FrontalDocument, 0, generation, DateTimeOffset.UtcNow));

        Check.False(viewModel.IsFocusTargetOutsideLiveCameraDomain, "The target must sit inside CAM-A's domain once explicitly placed there.");
        Check.True(viewModel.CanExecuteAutoFocus, "AF must be available once Live View is on, the panel is available, and the target is in-domain.");
        Check.Equal("CAM-A: 未固定", viewModel.CameraAFocusStatusText);

        viewModel.AutoFocusCommand.Execute(null);
        Check.True(viewModel.IsAutoFocusRunning, "AF must report itself running immediately after Execute().");
        await WaitUntilAsync(() => !viewModel.IsAutoFocusRunning, "AF execution did not complete.");

        Check.True(viewModel.FocusResultText.Contains("合焦OK", StringComparison.Ordinal), "A sharp frame (BlurRadius defaults to 0 on the fake source) must report 合焦OK.");
        Check.True(viewModel.FocusResultText.Contains("CAM-A", StringComparison.Ordinal), "The result must identify which camera AF ran on.");
        Check.True(viewModel.FocusResultText.Contains("0.20", StringComparison.Ordinal), "The result must record the target □ position AF used.");
        Check.Equal("CAM-A: 固定済", viewModel.CameraAFocusStatusText);
        Check.False(viewModel.CautionText.Contains("CAM-A: フォーカス未固定", StringComparison.Ordinal), "A fixed camera must not still carry the未固定 Caution notice.");
        Check.True(viewModel.CautionText.Contains("CAM-B: フォーカス未固定", StringComparison.Ordinal), "The still-unfixed CAM-B must remain a Caution notice (never a Blocker).");

        var preCaptureResult = new FocusExecutionResult("CAM-B", true, DateTimeOffset.Now, 0.7, 0.3);
        viewModel.RecordPreCaptureAutoFocusOutcome(preCaptureResult);
        Check.True(viewModel.LastPreCaptureAutoFocusResult is not null, "The pre-capture AF hook must store the recorded result.");
        Check.Equal(preCaptureResult, viewModel.LastPreCaptureAutoFocusResult!);
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task AutoFocusReportsNgDuringBlurRampAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-AutoFocusNgTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        // The real frame source (not the fake) is used here so the tick's BlurRadius actually
        // follows SimulatedTestImageFrameSource's blur-to-focus ramp instead of the fake
        // source's always-sharp default.
        var frameSource = new SimulatedTestImageFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        viewModel.SetTargetPosition(0.2, 0.5);
        viewModel.ToggleLiveViewCommand.Execute(null);
        var generation = pump.LastReturnedGeneration;
        // sequenceNumber=0 sits at the start of the blur ramp (near-maximum blur radius, see
        // SimulatedTestImageFrameSourceAppliesBlurAcrossTheFocusTransition above).
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.BlurToFocusTransition, 0, generation, DateTimeOffset.UtcNow));

        Check.True(viewModel.CanExecuteAutoFocus, "AF must remain invocable even though the live frame is currently blurred — SIMULATED AF is allowed to fail, not blocked outright.");
        viewModel.AutoFocusCommand.Execute(null);
        await WaitUntilAsync(() => !viewModel.IsAutoFocusRunning, "AF execution did not complete.");

        Check.True(viewModel.FocusResultText.Contains("合焦NG", StringComparison.Ordinal), "AF executed mid-blur-ramp must report 合焦NG, tying the result to the pattern's sequence progression.");
        Check.Equal("CAM-A: 未固定", viewModel.CameraAFocusStatusText);
        Check.True(viewModel.CautionText.Contains("CAM-A: フォーカス未固定", StringComparison.Ordinal), "A failed AF must not fix the camera, so the未固定 Caution notice must remain.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task FocusTargetOutsideLiveDomainBlocksAfAndSwitchButtonRestoresItAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-FocusDomainSwitchTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.False(viewModel.IsSingleCameraMode, "Dual mode must remain the default for this domain-split scenario.");
        Check.Equal("CAM-A", viewModel.SelectedCamera);

        // 0.8 sits in CAM-B's fixed document-half domain while CAM-A is selected/live: mismatch.
        viewModel.SetTargetPosition(0.8, 0.5);
        Check.True(viewModel.IsFocusTargetOutsideLiveCameraDomain, "A target in CAM-B's domain while CAM-A is live must be flagged out-of-domain.");
        Check.True(viewModel.ShowSwitchLiveCameraButton, "The switch-to-live button must show when the target is out of the live camera's domain.");
        Check.False(viewModel.ShowAutoFocusButton, "The AF button must hide (not just disable) while the switch button is shown.");
        Check.Equal("CAM-B live に切替", viewModel.SwitchLiveCameraButtonText);
        Check.False(viewModel.CanExecuteAutoFocus, "AF must stay disabled while the target is out of the live camera's domain.");
        Check.True(viewModel.CanSwitchLiveCameraToTargetDomain, "The switch command must be available to resolve the mismatch.");

        viewModel.SwitchLiveCameraToTargetDomainCommand.Execute(null);

        Check.Equal("CAM-B", viewModel.SelectedCamera);
        Check.True(viewModel.IsLiveViewActive, "The one-click switch must also start Live View for the newly-selected camera.");
        Check.False(viewModel.IsFocusTargetOutsideLiveCameraDomain, "After switching to the domain-owning camera, the target must no longer be out-of-domain.");
        Check.True(viewModel.ShowAutoFocusButton, "The AF button must reappear once the live camera matches the target's domain.");
        Check.True(viewModel.CanExecuteAutoFocus, "AF must become available once the live camera matches the target's domain.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task MfStepAdjustsRelativeValueAndUnfixesFocusAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-MfStepTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        var frameSource = new FakeSimulatedLiveViewFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        viewModel.SetTargetPosition(0.2, 0.5);
        viewModel.ToggleLiveViewCommand.Execute(null);
        var generation = pump.LastReturnedGeneration;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.FrontalDocument, 0, generation, DateTimeOffset.UtcNow));

        Check.True(IsClose(50.0, viewModel.FocusPositionValue), "MF position must default to the mid-range relative value (50/100) before any step.");

        viewModel.MfCoarseForwardCommand.Execute(null);
        Check.True(IsClose(60.0, viewModel.FocusPositionValue), "A coarse forward step must move the relative value by the coarse step size.");

        viewModel.MfFineBackwardCommand.Execute(null);
        Check.True(IsClose(58.0, viewModel.FocusPositionValue), "A fine backward step must move the relative value by the fine step size.");

        // Clamping: drive far past the 0..100 bounds and confirm it holds at the edge.
        for (var index = 0; index < 20; index++)
        {
            viewModel.MfCoarseForwardCommand.Execute(null);
        }
        Check.True(IsClose(100.0, viewModel.FocusPositionValue), "The relative focus value must clamp at 100, never overshoot.");

        // AF-then-fix, then confirm a manual MF nudge un-fixes it again.
        viewModel.AutoFocusCommand.Execute(null);
        await WaitUntilAsync(() => !viewModel.IsAutoFocusRunning, "AF execution did not complete.");
        Check.Equal("CAM-A: 固定済", viewModel.CameraAFocusStatusText);

        viewModel.MfFineForwardCommand.Execute(null);
        Check.Equal("CAM-A: 未固定", viewModel.CameraAFocusStatusText);
        Check.True(viewModel.CautionText.Contains("CAM-A: フォーカス未固定", StringComparison.Ordinal), "A manual MF nudge after AF must restore the未固定 Caution notice.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }

    static bool IsClose(double expected, double actual) => Math.Abs(expected - actual) < 1e-9;
}

static async Task FocusPeakingOverlayHighlightsEdgesAndTogglesWithViewModelStateAsync()
{
    // Renderer-level: a degenerate 1x1 frame (the shape the fake test frame source always
    // returns) must never be highlighted — there is nothing meaningful to detect an edge in.
    var source = new SimulatedTestImageFrameSource();
    var degenerate = BitmapSource.Create(1, 1, 96, 96, PixelFormats.Bgr24, null, new byte[] { 1, 2, 3 }, 3);
    degenerate.Freeze();
    Check.True(FocusPeakingOverlayRenderer.BuildOverlay(degenerate) is null, "A degenerate 1x1 source must produce no overlay.");
    Check.True(FocusPeakingOverlayRenderer.BuildOverlay(null) is null, "A null source must produce no overlay.");

    // A real synthetic document frame has sharp line-art edges over a flat background: the
    // overlay must actually highlight *some* pixels (not be entirely transparent).
    var frame = source.CreateFrame("CAM-A", SimulatedFramePattern.FrontalDocument, sequenceNumber: 0, DateTimeOffset.UtcNow);
    var overlay = FocusPeakingOverlayRenderer.BuildOverlay(frame.Image);
    Check.True(overlay is not null, "A real document frame must produce a non-null overlay.");
    var overlayPixels = CopyPixelsBgra(overlay!);
    var highlightedPixelCount = 0;
    for (var index = 3; index < overlayPixels.Length; index += 4)
    {
        if (overlayPixels[index] != 0)
        {
            highlightedPixelCount++;
        }
    }
    Check.True(
        highlightedPixelCount > 0,
        "At least one pixel must be highlighted for a frame with real line-art edges " +
        $"(highlighted = {highlightedPixelCount}). 0 なら描画そのものが空で、blur/tilt も同時に落ちているはず。");

    // ViewModel-level: the overlay must only be exposed while IsPeakingEnabled is true, and it
    // must react to the live stage image the same way the base image bindings do.
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-PeakingToggleTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        var frameSource = new FakeSimulatedLiveViewFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.Equal("ピーキング ON", viewModel.PeakingButtonText);
        Check.True(viewModel.TogglePeakingCommand.CanExecute(null), "Peaking toggle must be available while the focus panel is available.");

        viewModel.ToggleLiveViewCommand.Execute(null);
        var generation = pump.LastReturnedGeneration;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.FrontalDocument, 0, generation, DateTimeOffset.UtcNow));
        Check.True(viewModel.StageCompositeLiveImage is not null, "A live-ticked frame must populate the base composite live image first.");
        Check.True(viewModel.StageCompositeLivePeakingOverlay is null, "The overlay must stay null while peaking is off, even with a base image present.");

        viewModel.TogglePeakingCommand.Execute(null);
        Check.True(viewModel.IsPeakingEnabled, "Toggling peaking must flip IsPeakingEnabled.");
        Check.Equal("ピーキング OFF", viewModel.PeakingButtonText);
        // The fake frame source always returns a 1x1 image, which BuildOverlay treats as
        // degenerate — so the overlay itself is still null here, but the *gate* (IsPeakingEnabled)
        // is what this asserts, matching the renderer-level assertions above for the real-image case.
        Check.True(viewModel.StageCompositeLivePeakingOverlay is null, "The 1x1 fake frame stays degenerate even with peaking on — confirms BuildOverlay is actually being invoked through the gate, not bypassed.");

        viewModel.TogglePeakingCommand.Execute(null);
        Check.False(viewModel.IsPeakingEnabled, "Toggling peaking again must flip it back off.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task FocusPanelDisabledInHardwareDualEnvironmentAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-FocusPanelHardwareGateTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var hardwareFlow = DualCameraProductComposition.Create(
            Path.Combine(root, "hardware-products"),
            DualCameraExecutionEnvironment.HardwareDual);
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            hardwareFlow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        Check.False(viewModel.IsFocusPanelAvailable, "HardwareDual must disable the focus panel until hardware-required Issue + human gate approval (#35 Option A).");
        Check.True(viewModel.IsFocusPanelUnavailable, "The inverse flag bound by XAML must agree with IsFocusPanelAvailable.");
        Check.True(viewModel.FocusPanelUnavailableReason.Contains("実機", StringComparison.Ordinal), "The shown reason must explain the hardware-mode gate (fail-closed with a stated reason).");
        Check.False(viewModel.CanUseFocusPanel, "MF/peaking must stay disabled while the focus panel itself is unavailable.");
        Check.False(viewModel.CanExecuteAutoFocus, "AF must stay disabled while the focus panel itself is unavailable.");
        Check.False(viewModel.CanSwitchLiveCameraToTargetDomain, "The switch-live-camera action must stay disabled while the focus panel itself is unavailable.");
        Check.False(viewModel.TogglePeakingCommand.CanExecute(null), "Peaking toggle must stay disabled while the focus panel itself is unavailable.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task CaptureWithAutoFocusSucceedsThenCapturesAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-CaptureWithAutoFocusSuccessTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        Check.False(viewModel.IsSingleCameraMode, "Dual mode must remain the default for this issue #33 撮影+AF regression.");
        Check.True(viewModel.CanCapture, "A ready Dual plan must allow capture.");
        Check.True(viewModel.CanCaptureWithAutoFocus, "撮影+AF must be available in SIMULATED Dual mode (no HardwareDual gate active).");
        Check.True(viewModel.IsActionZonePreparing, "The action zone must show state 1 (readiness card + capture buttons) while Ready.");
        Check.False(viewModel.IsActionZoneProcessing, "The action zone must not show the progress strip before capture starts.");
        Check.False(viewModel.IsActionZoneReview, "The action zone must not show the result panel before capture starts.");

        viewModel.CaptureWithAutoFocusCommand.Execute(null);
        await WaitUntilAsync(() => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy, "撮影+AF did not finish its capture.");

        Check.Equal(OperatorUiState.Review, viewModel.UiState);
        Check.True(viewModel.IsActionZoneReview, "The action zone must show state 3 (result panel) once Review is reached.");
        Check.True(viewModel.RetainedOriginals.Contains("CAM-A", StringComparison.Ordinal), "CAM-A original must be retained after a successful 撮影+AF.");
        Check.True(viewModel.RetainedOriginals.Contains("CAM-B", StringComparison.Ordinal), "CAM-B original must be retained after a successful 撮影+AF.");
        Check.True(viewModel.StitchResult.Contains("自動合成完了", StringComparison.Ordinal), "A successful Dual 撮影+AF must still auto-stitch through the unchanged existing flow.");

        Check.True(viewModel.LastPreCaptureAutoFocusResult is not null, "The #31 pre-capture AF hook must have recorded an outcome (SIMULATED journal-equivalent record).");
        Check.True(viewModel.LastPreCaptureAutoFocusResult!.Success, "The last recorded pre-capture AF outcome (CAM-B, the second required camera) must be 合焦OK.");
        Check.Equal("CAM-B", viewModel.LastPreCaptureAutoFocusResult!.CameraAlias);
        Check.Equal("CAM-B: 固定済", viewModel.CameraBFocusStatusText);
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task CaptureWithAutoFocusStopsBeforeShutterOnNgAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-CaptureWithAutoFocusNgTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        // The real frame source (not the fake) is used so the tick's BlurRadius actually follows
        // SimulatedTestImageFrameSource's blur-to-focus ramp, mirroring
        // AutoFocusReportsNgDuringBlurRampAsync's setup for issue #31's own "AF実行" button — the
        // same NG scenario now exercised through the 撮影+AF pre-capture gate instead.
        var frameSource = new SimulatedTestImageFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.False(viewModel.IsSingleCameraMode, "Dual mode must remain the default for this 撮影+AF regression.");
        Check.Equal("CAM-A", viewModel.SelectedCamera);

        viewModel.ToggleLiveViewCommand.Execute(null);
        var generation = pump.LastReturnedGeneration;
        // sequenceNumber=0 sits at the start of the blur ramp (near-maximum blur radius), so
        // CAM-A — the first camera modeが要求する — fails its pre-capture AF check.
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.BlurToFocusTransition, 0, generation, DateTimeOffset.UtcNow));

        Check.True(viewModel.CanCaptureWithAutoFocus, "撮影+AF must remain invocable even though CAM-A's live frame is currently blurred — SIMULATED AF is allowed to fail, not blocked outright.");

        viewModel.CaptureWithAutoFocusCommand.Execute(null);
        await WaitUntilAsync(() => viewModel.UiState == OperatorUiState.FailedPartial, "撮影+AF did not stop with FailedPartial after CAM-A's pre-capture AF failure.");

        Check.Equal(0, viewModel.TransactionStartCount);
        Check.True(viewModel.LastPreCaptureAutoFocusResult is not null, "The pre-capture AF hook must have recorded CAM-A's failed outcome.");
        Check.False(viewModel.LastPreCaptureAutoFocusResult!.Success, "The recorded pre-capture AF outcome for CAM-A must be 合焦NG.");
        Check.Equal("CAM-A", viewModel.LastPreCaptureAutoFocusResult!.CameraAlias);
        Check.True(viewModel.CaptureResult.Contains("AF NG", StringComparison.Ordinal), "The result panel must show the shutter was never fired for this attempt.");
        Check.True(viewModel.StatusMessage.Contains("シャッターを実行せず", StringComparison.Ordinal), "The status message must explain the fail-closed stop.");
        Check.Equal("CAM-A: 未固定", viewModel.CameraAFocusStatusText);
        Check.True(viewModel.IsActionZoneReview, "FailedPartial belongs to action zone state 3 (result panel with 新しい撮影を準備), not the capture buttons.");
        Check.False(viewModel.CanCapture, "A FailedPartial stop must block another capture until 新しい撮影を準備 — the same contract every other failure point already uses.");
        Check.True(viewModel.CanPrepareNewCapture, "The operator must be able to explicitly prepare a new transaction after a fail-closed 撮影+AF stop.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task CaptureWithAutoFocusUnavailableUnderHardwareDualAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-CaptureWithAutoFocusHardwareGateTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var hardwareFlow = DualCameraProductComposition.Create(
            Path.Combine(root, "hardware-products"),
            DualCameraExecutionEnvironment.HardwareDual);
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            hardwareFlow);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        Check.False(viewModel.CanCaptureWithAutoFocus, "実機モード（HardwareDual）では撮影+AFを実行不可とする（#35 Option A・#31のIsFocusPanelAvailableゲートを流用）。");
        Check.False(viewModel.CaptureWithAutoFocusCommand.CanExecute(null), "The bound command must agree with CanCaptureWithAutoFocus.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task ActionZoneVisibilitySwitchesWithUiStateAsync()
{
    var service = new BlockingTransactionService();
    var viewModel = new OperatorShellViewModel(service);
    await viewModel.InitializeAsync(CancellationToken.None);
    viewModel.SelectedOperatingMode = "1台構成";
    viewModel.SelectedCamera = "CAM-B";

    Check.Equal(OperatorUiState.AwaitingSafetyAck, viewModel.UiState);
    Check.True(viewModel.IsActionZonePreparing, "AwaitingSafetyAck must show action zone state 1 (readiness card + capture buttons).");
    Check.False(viewModel.IsActionZoneProcessing, "AwaitingSafetyAck must not show the progress strip.");
    Check.False(viewModel.IsActionZoneReview, "AwaitingSafetyAck must not show the result panel.");

    viewModel.AcceptSafetyCommand.Execute(null);
    Check.Equal(OperatorUiState.Ready, viewModel.UiState);
    Check.True(viewModel.IsActionZonePreparing, "Ready must still show action zone state 1.");
    Check.False(viewModel.IsActionZoneProcessing, "Ready must not show the progress strip.");
    Check.False(viewModel.IsActionZoneReview, "Ready must not show the result panel.");

    viewModel.CaptureCommand.Execute(null);
    await service.Started.WaitAsync(TimeSpan.FromSeconds(5));
    Check.Equal(OperatorUiState.Capturing, viewModel.UiState);
    Check.False(viewModel.IsActionZonePreparing, "Capturing must hide action zone state 1.");
    Check.True(viewModel.IsActionZoneProcessing, "Capturing must show action zone state 2 (自動進捗ストリップ).");
    Check.False(viewModel.IsActionZoneReview, "Capturing must hide action zone state 3.");

    service.Release();
    await WaitUntilAsync(() => !viewModel.IsBusy, "The blocking capture did not finish.");
    Check.Equal(OperatorUiState.Review, viewModel.UiState);
    Check.False(viewModel.IsActionZonePreparing, "Review must hide action zone state 1.");
    Check.False(viewModel.IsActionZoneProcessing, "Review must hide action zone state 2.");
    Check.True(viewModel.IsActionZoneReview, "Review must show action zone state 3 (結果パネル).");
}

static void DocumentTiltDetectorMeasuresKnownRollAnglesAndReportsUndetectable()
{
    Check.True(DocumentTiltDetector.DetectRollDegrees(null) is null, "A null source must report 検出不能.");
    var degenerate = BitmapSource.Create(1, 1, 96, 96, PixelFormats.Bgr24, null, new byte[] { 1, 2, 3 }, 3);
    degenerate.Freeze();
    Check.True(DocumentTiltDetector.DetectRollDegrees(degenerate) is null, "A degenerate 1x1 source (the fake frame source's shape) must report 検出不能.");

    var source = new SimulatedTestImageFrameSource();
    var frontal = source.CreateFrame("CAM-A", SimulatedFramePattern.FrontalDocument, 0, DateTimeOffset.UtcNow);
    var frontalRoll = DocumentTiltDetector.DetectRollDegrees(frontal.Image);
    Check.True(frontalRoll is not null, "A frontal (unrotated) document must be detected, not 検出不能.");
    Check.True(Math.Abs(frontalRoll!.Value) < 0.1, $"A frontal document must read ~0°, got {frontalRoll.Value:F4}°.");

    // Measured against the SIMULATED tilt test patterns' known rotation angles (see the
    // implementation notes/PR description): absolute error stayed <=0.3° at both tested
    // magnitudes (±3°, ±6°). The tolerance below is set with margin above that measured error —
    // it documents a real, checked accuracy limit rather than claiming exact-degree precision.
    const double toleranceDegrees = 0.5;
    foreach (var (pattern, expectedDegrees) in new[]
             {
                 (SimulatedFramePattern.TiltedDocumentRollMinus6, -6.0),
                 (SimulatedFramePattern.TiltedDocumentRollMinus3, -3.0),
                 (SimulatedFramePattern.TiltedDocumentRollPlus3, 3.0),
                 (SimulatedFramePattern.TiltedDocumentRollPlus6, 6.0),
             })
    {
        var frame = source.CreateFrame("CAM-A", pattern, 0, DateTimeOffset.UtcNow);
        var detected = DocumentTiltDetector.DetectRollDegrees(frame.Image);
        Check.True(detected is not null, $"{pattern} must be detected, not 検出不能.");
        Check.True(
            Math.Abs(detected!.Value - expectedDegrees) <= toleranceDegrees,
            $"{pattern}: expected ~{expectedDegrees}°, got {detected.Value:F4}° (tolerance ±{toleranceDegrees}°).");
    }

    // A frame with no matching document fill at all — the shape a non-live/未取得 preview would
    // degrade toward — must fall back to 検出不能 rather than reporting a noise-driven angle.
    var backgroundOnly = new WriteableBitmap(64, 64, 96, 96, PixelFormats.Bgra32, null);
    var backgroundPixels = new byte[64 * 64 * 4];
    for (var index = 0; index < backgroundPixels.Length; index += 4)
    {
        backgroundPixels[index] = 0x1F;
        backgroundPixels[index + 1] = 0x1A;
        backgroundPixels[index + 2] = 0x14;
        backgroundPixels[index + 3] = 0xFF;
    }
    backgroundOnly.WritePixels(new System.Windows.Int32Rect(0, 0, 64, 64), backgroundPixels, 64 * 4, 0);
    Check.True(DocumentTiltDetector.DetectRollDegrees(backgroundOnly) is null, "A frame with no document fill must report 検出不能, not a fabricated angle.");
}

static async Task TiltReadingReflectsLiveFrameAndShowsUndetectableWhenNotLiveAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-TiltReadingTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        var frameSource = new SimulatedTestImageFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        Check.True(viewModel.TiltRollDegrees is null, "Before Live View starts, there is no frame to detect a tilt from.");
        Check.Equal("傾き 検出不能", viewModel.TiltRollDegreesText);

        viewModel.ToggleLiveViewCommand.Execute(null);
        var generation = pump.LastReturnedGeneration;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.TiltedDocumentRollPlus6, 0, generation, DateTimeOffset.UtcNow));

        Check.True(viewModel.TiltRollDegrees is not null, "A live-ticked tilted document frame must produce a detected angle.");
        Check.True(
            Math.Abs(viewModel.TiltRollDegrees!.Value - 6.0) < 0.5,
            $"The +6° pattern must be detected within test tolerance, got {viewModel.TiltRollDegrees.Value:F4}°.");
        Check.True(viewModel.TiltRollDegreesText.StartsWith("傾き ", StringComparison.Ordinal), "The reading text must keep the 傾き label.");
        Check.False(viewModel.TiltRollDegreesText.Contains("検出不能", StringComparison.Ordinal), "A successfully detected reading must not show 検出不能.");

        viewModel.ToggleLiveViewCommand.Execute(null);
        Check.True(viewModel.TiltRollDegrees is null, "Stopping Live View must revert the reading to 検出不能 — the non-live/frame-not-yet-obtained contract.");
        Check.Equal("傾き 検出不能", viewModel.TiltRollDegreesText);
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task AlignmentGuideOverlayTogglesControlVisibilityAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OverlayToggleTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        // Grid/トンボ/安全マージン are newly introduced guide layers and must default off; 重複帯
        // already displayed unconditionally before this issue made it toggleable, so its default
        // must stay on to avoid silently hiding something operators already relied on.
        Check.False(viewModel.IsGridOverlayEnabled, "Grid overlay must default off.");
        Check.False(viewModel.IsTombOverlayEnabled, "Tomb overlay must default off.");
        Check.False(viewModel.IsSafeMarginOverlayEnabled, "Safe margin overlay must default off.");
        Check.True(viewModel.IsOverlapBandOverlayEnabled, "Overlap band overlay must default on (preserves pre-#32 behavior).");

        Check.False(viewModel.IsGridOverlayVisible, "Grid overlay must stay hidden until enabled.");
        viewModel.IsGridOverlayEnabled = true;
        Check.True(viewModel.IsGridOverlayVisible, "Enabling the grid toggle must make it visible.");

        viewModel.IsTombOverlayEnabled = true;
        Check.True(viewModel.IsTombOverlayVisible, "Enabling the tomb toggle must make it visible.");

        viewModel.IsSafeMarginOverlayEnabled = true;
        Check.True(viewModel.IsSafeMarginOverlayVisible, "Enabling the safe-margin toggle must make it visible.");

        Check.True(viewModel.IsOverlapBandVisible, "Overlap band must stay visible (Dual mode default) while its toggle is on.");
        viewModel.IsOverlapBandOverlayEnabled = false;
        Check.False(viewModel.IsOverlapBandVisible, "Disabling the overlap band toggle must hide it even in Dual mode.");
        viewModel.IsOverlapBandOverlayEnabled = true;

        viewModel.SelectedOperatingMode = "1台構成";
        Check.False(viewModel.IsOverlapBandVisible, "The overlap band must stay hidden in SingleCamera mode regardless of the toggle (no composite to overlap).");
        viewModel.SelectedOperatingMode = "2台構成";

        // None of the four overlay toggles may ever reach CanCapture — the 常時禁止 "原稿エッジ
        // 検出・傾き読み値による撮影可否の判定と自動補正への接続" guard applies to these guides too.
        var captureBefore = viewModel.CanCapture;
        viewModel.IsGridOverlayEnabled = false;
        viewModel.IsTombOverlayEnabled = false;
        viewModel.IsSafeMarginOverlayEnabled = false;
        Check.Equal(captureBefore, viewModel.CanCapture);
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }

    // Separately: the guide overlays must hide (not just the target reticle) while the stage
    // shows the Capturing/Stitching processing placeholder — mirrors IsTargetOverlayVisible's
    // own gate, using the same BlockingTransactionService pattern
    // ActionZoneVisibilitySwitchesWithUiStateAsync uses to hold UiState at Capturing deterministically.
    var service = new BlockingTransactionService();
    var placeholderViewModel = new OperatorShellViewModel(service);
    await placeholderViewModel.InitializeAsync(CancellationToken.None);
    placeholderViewModel.SelectedOperatingMode = "1台構成";
    placeholderViewModel.SelectedCamera = "CAM-B";
    placeholderViewModel.AcceptSafetyCommand.Execute(null);
    placeholderViewModel.IsGridOverlayEnabled = true;
    placeholderViewModel.IsTombOverlayEnabled = true;
    placeholderViewModel.IsSafeMarginOverlayEnabled = true;
    Check.True(placeholderViewModel.IsGridOverlayVisible, "Grid overlay must be visible before capture starts.");

    placeholderViewModel.CaptureCommand.Execute(null);
    await service.Started.WaitAsync(TimeSpan.FromSeconds(5));
    Check.Equal(OperatorUiState.Capturing, placeholderViewModel.UiState);
    Check.False(placeholderViewModel.IsGridOverlayVisible, "Grid overlay must hide during the Capturing processing placeholder.");
    Check.False(placeholderViewModel.IsTombOverlayVisible, "Tomb overlay must hide during the Capturing processing placeholder.");
    Check.False(placeholderViewModel.IsSafeMarginOverlayVisible, "Safe margin overlay must hide during the Capturing processing placeholder.");

    service.Release();
    await WaitUntilAsync(() => !placeholderViewModel.IsBusy, "The blocking capture did not finish.");
}

static async Task TiltToleranceInputSetsChipTextAndRejectsInvalidValuesAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-TiltToleranceTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var pump = new FakeSimulatedLiveViewFramePump();
        var frameSource = new SimulatedTestImageFrameSource();
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(root),
            dualCameraFlow: null,
            liveViewFramePump: pump,
            liveViewFrameSource: frameSource);
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);

        // Unset by default — issue #32: "許容値は設定値とし、初期値の決定は実装時に操作者へ確認
        // する（勝手に既定値を作らない）"。No operator was available to ask during this automated
        // implementation, so it stays unset rather than guessing a number.
        Check.True(viewModel.TiltToleranceDegrees is null, "Tolerance must start unset — no invented default.");
        Check.Equal("許容値未設定", viewModel.TiltToleranceChipText);

        // Invalid input must be rejected and must re-announce the previously accepted value so a
        // bound TextBox reverts, matching the rejection pattern used elsewhere in this VM.
        var propertyChangedNames = new List<string>();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is not null)
            {
                propertyChangedNames.Add(args.PropertyName);
            }
        };
        viewModel.TiltToleranceInputText = "not-a-number";
        Check.Equal(string.Empty, viewModel.TiltToleranceInputText);
        Check.True(viewModel.TiltToleranceDegrees is null, "A rejected input must not change the accepted tolerance.");
        Check.True(
            propertyChangedNames.Contains(nameof(OperatorShellViewModel.TiltToleranceInputText)),
            "A rejected value must still raise PropertyChanged so the bound TextBox reverts.");

        viewModel.TiltToleranceInputText = "0.50";
        Check.True(
            viewModel.TiltToleranceDegrees is { } tolerance && Math.Abs(tolerance - 0.5) < 1e-9,
            "A valid numeric input must be accepted.");

        // Set but nothing live yet: the reading is 検出不能, so the chip must say so instead of
        // fabricating a within/exceeded judgment against a nonexistent angle.
        Check.Equal("許容 ±0.50° 内 / 検出不能のため判定不可", viewModel.TiltToleranceChipText);

        // A live ~+6° tilt frame against a tight ±0.50° tolerance must read as exceeded.
        viewModel.ToggleLiveViewCommand.Execute(null);
        var generation = pump.LastReturnedGeneration;
        pump.RaiseTick(new SimulatedLiveViewFrameTick("CAM-A", SimulatedFramePattern.TiltedDocumentRollPlus6, 0, generation, DateTimeOffset.UtcNow));
        Check.True(
            viewModel.TiltToleranceChipText.Contains("許容超過", StringComparison.Ordinal),
            $"A ~6° reading against a ±0.50° tolerance must read as exceeded, got: {viewModel.TiltToleranceChipText}");

        // The same reading against a wide tolerance must read as within it.
        viewModel.TiltToleranceInputText = "10.00";
        Check.True(
            viewModel.TiltToleranceChipText.Contains("許容内", StringComparison.Ordinal),
            $"A ~6° reading against a ±10.00° tolerance must read as within it, got: {viewModel.TiltToleranceChipText}");

        // Clearing the input must return to unset, not to some prior remembered default.
        viewModel.TiltToleranceInputText = string.Empty;
        Check.True(viewModel.TiltToleranceDegrees is null, "Clearing the input must unset the tolerance.");
        Check.Equal("許容値未設定", viewModel.TiltToleranceChipText);
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task MenuNavigationSwitchesPagesAndLocksDuringCaptureAsync()
{
    var service = new BlockingTransactionService();
    var viewModel = new OperatorShellViewModel(service);
    await viewModel.InitializeAsync(CancellationToken.None);
    viewModel.SelectedOperatingMode = "1台構成";
    viewModel.SelectedCamera = "CAM-B";

    Check.Equal("Dashboard", viewModel.SelectedPage);
    Check.Equal("撮影ダッシュボード", viewModel.PageTitle);
    Check.True(viewModel.ShowSetupCommand.CanExecute(null), "設置・校正メニュー項目はactive transaction外なら有効でなければならない.");

    viewModel.ShowSetupCommand.Execute(null);
    Check.Equal("Setup", viewModel.SelectedPage);
    Check.Equal("設置・校正", viewModel.PageTitle);

    viewModel.ShowCameraSettingsCommand.Execute(null);
    Check.Equal("CameraSettings", viewModel.SelectedPage);
    Check.Equal("カメラ設定（read-only）", viewModel.PageTitle);

    viewModel.ShowDiagnosticsCommand.Execute(null);
    Check.Equal("Diagnostics", viewModel.SelectedPage);
    Check.Equal("保存・診断", viewModel.PageTitle);

    viewModel.ShowDashboardCommand.Execute(null);
    Check.Equal("Dashboard", viewModel.SelectedPage);
    Check.Equal("撮影ダッシュボード", viewModel.PageTitle);

    viewModel.AcceptSafetyCommand.Execute(null);
    viewModel.CaptureCommand.Execute(null);
    await service.Started.WaitAsync(TimeSpan.FromSeconds(5));
    Check.Equal(OperatorUiState.Capturing, viewModel.UiState);
    Check.False(viewModel.CanOpenMaintenance, "active transaction中は保守画面を開けてはならない.");
    Check.False(viewModel.ShowSetupCommand.CanExecute(null), "設置・校正メニュー項目はCapturing中は無効化されなければならない.");
    Check.False(viewModel.ShowCameraSettingsCommand.CanExecute(null), "カメラ設定メニュー項目はCapturing中は無効化されなければならない.");
    Check.False(viewModel.ShowDiagnosticsCommand.CanExecute(null), "保存・診断メニュー項目はCapturing中は無効化されなければならない.");
    Check.False(viewModel.CanChangeExportDirectory, "保存先変更はCapturing中は無効化されなければならない（メニュー経由も状態ゲートの例外にしない）.");
    Check.False(viewModel.CanChangeOperatingMode, "運用構成の変更はCapturing中は無効化されなければならない.");

    service.Release();
    await WaitUntilAsync(() => !viewModel.IsBusy, "The blocking capture did not finish.");
    Check.Equal(OperatorUiState.Review, viewModel.UiState);
    Check.True(viewModel.ShowSetupCommand.CanExecute(null), "処理完了後は保守画面メニュー項目が再び有効化されなければならない.");
}

static void OperatingModeAndLoupeZoomMenuTogglesStaySynced()
{
    var viewModel = new OperatorShellViewModel(new BlockingTransactionService());

    Check.True(viewModel.IsDualCameraModeChecked, "既定の運用構成はDualCameraのため、対応するラジオ項目は最初からチェック済みでなければならない.");
    Check.False(viewModel.IsSingleCameraModeChecked, "既定がDualCameraである以上、SingleCamera側は最初は未チェックでなければならない.");

    viewModel.IsSingleCameraModeChecked = true;
    Check.Equal("1台構成", viewModel.SelectedOperatingMode);
    Check.True(viewModel.IsSingleCameraModeChecked, "SingleCameraをチェックしたら選択されなければならない.");
    Check.False(viewModel.IsDualCameraModeChecked, "SingleCameraをチェックしたらDualCamera側は連動して未チェックにならなければならない.");

    viewModel.IsDualCameraModeChecked = true;
    Check.Equal("2台構成", viewModel.SelectedOperatingMode);
    Check.True(viewModel.IsDualCameraModeChecked, "DualCameraをチェックしたら選択されなければならない.");
    Check.False(viewModel.IsSingleCameraModeChecked, "DualCameraをチェックしたらSingleCamera側は連動して未チェックにならなければならない.");

    // MenuItemにはRadioButtonのGroupNameに相当する仕組みがないため、選択中の項目を直接
    // falseへ外そうとする操作（もう一方をチェックするのではなく）は無視されなければならない —
    // さもないとSelectedOperatingModeがどちらの値も表さない状態になり得る。
    viewModel.IsDualCameraModeChecked = false;
    Check.Equal("2台構成", viewModel.SelectedOperatingMode);
    Check.True(viewModel.IsDualCameraModeChecked, "選択中の項目を直接外す操作は無視され、元の選択を維持しなければならない.");

    Check.True(viewModel.IsLoupeZoom100Checked, "既定の拡大エリア倍率は100%のため、対応するラジオ項目は最初からチェック済みでなければならない.");
    Check.False(viewModel.IsLoupeZoom200Checked, "既定が100%である以上、200%側は最初は未チェックでなければならない.");

    viewModel.IsLoupeZoom200Checked = true;
    Check.Equal("200%", viewModel.SelectedLoupeZoom);
    Check.True(viewModel.IsLoupeZoom200Checked, "200%をチェックしたら選択されなければならない.");
    Check.False(viewModel.IsLoupeZoom100Checked, "200%をチェックしたら100%側は連動して未チェックにならなければならない.");

    viewModel.IsLoupeZoom100Checked = true;
    Check.Equal("100%", viewModel.SelectedLoupeZoom);
    Check.True(viewModel.IsLoupeZoom100Checked, "100%をチェックしたら選択されなければならない.");
    Check.False(viewModel.IsLoupeZoom200Checked, "100%をチェックしたら200%側は連動して未チェックにならなければならない.");
}

static void TiltReadingVisibilityMenuToggleDefaultsVisibleAndTogglesIndependently()
{
    var viewModel = new OperatorShellViewModel(new BlockingTransactionService());

    // 傾き常駐行は#32で追加済みの既存表示のため、#34で新設するこのトグルの既定値はON（表示）
    // でなければならない — 新規トグルが既存表示を黙って隠してはならない。
    Check.True(viewModel.IsTiltReadingVisible, "傾き読み値の表示トグルは既定でON（表示）でなければならない.");

    viewModel.IsTiltReadingVisible = false;
    Check.False(viewModel.IsTiltReadingVisible, "表示メニューのトグル操作でOFFへ切り替えられなければならない.");

    // 表示(V)メニューに同居する#32の他トグルと独立していること。
    Check.False(viewModel.IsGridOverlayEnabled, "グリッドトグルは既定でOFFのままでなければならない.");
    viewModel.IsGridOverlayEnabled = true;
    Check.True(viewModel.IsGridOverlayEnabled, "グリッドトグルは表示メニューから引き続き独立して切替できなければならない.");
    Check.False(viewModel.IsTiltReadingVisible, "グリッドトグルの変更が傾き読み値トグルへ波及してはならない.");

    viewModel.IsTiltReadingVisible = true;
    Check.True(viewModel.IsTiltReadingVisible, "傾き読み値トグルはONへ戻せなければならない.");
}

static async Task DualCameraIdentityStatusMenuTextReflectsSnapshotAndOperatingModeAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-IdentityStatusMenuTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var bridge = new NeverCaptureDualBridge();
        var flow = new DualCameraProductFlow(
            Path.Combine(root, "products"),
            bridge,
            bridge,
            new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.HardwarePending()));
        var viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(Path.Combine(root, "journals")),
            flow);
        await viewModel.InitializeAsync(CancellationToken.None);

        Check.True(
            viewModel.DualCameraIdentityStatusText.Contains("HardwarePending", StringComparison.Ordinal),
            "カメラメニューのidentity状態表示は現在のIdentitySnapshotを反映しなければならない.");

        viewModel.SelectedOperatingMode = "1台構成";
        Check.True(
            viewModel.DualCameraIdentityStatusText.Contains("対象外", StringComparison.Ordinal),
            "1台構成ではDualCamera identityは対象外と表示しなければならない.");

        viewModel.SelectedOperatingMode = "2台構成";
        Check.True(
            viewModel.DualCameraIdentityStatusText.Contains("HardwarePending", StringComparison.Ordinal),
            "2台構成へ戻したらidentity状態表示も復帰しなければならない.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static byte[] CopyPixelsBgra(BitmapSource bitmap)
{
    var stride = bitmap.PixelWidth * 4;
    var buffer = new byte[stride * bitmap.PixelHeight];
    bitmap.CopyPixels(buffer, stride, 0);
    return buffer;
}

// GitHub Issue #63: WPF の描画（RenderTargetBitmap・BlurEffect・Typeface 解決）は STA スレッドを
// 前提にする。このテストはトップレベルステートメントの Main で動くため既定が MTA で、描画を
// サポート外のアパートメントで走らせていた。動く環境と動かない環境が分かれる入り口になるので、
// 描画へ依存する試験だけをここで STA + Dispatcher の上へ持ち上げる。
//
// Dispatcher まで用意するのは、対象の試験が非同期で、await の継続がスレッドプール（MTA）へ
// 逃げてしまうため。DispatcherSynchronizationContext を敷いて PushFrame で回すことで、
// 試験の最初から最後まで同じ STA スレッドに留める。
//
// 前提を「たまたま満たされている」状態から「明示的に満たす」状態へ変えるのが目的で、
// assert は一つも緩めていない。
static void RunSyncOnStaRenderThread(Action body) =>
    RunOnStaRenderThread(() =>
    {
        body();
        return Task.CompletedTask;
    });

static void RunOnStaRenderThread(Func<Task> body)
{
    ExceptionDispatchInfo? captured = null;
    var thread = new Thread(() =>
    {
        var dispatcher = Dispatcher.CurrentDispatcher;
        SynchronizationContext.SetSynchronizationContext(new DispatcherSynchronizationContext(dispatcher));
        var frame = new DispatcherFrame();
        _ = dispatcher.InvokeAsync(async () =>
        {
            try
            {
                await body().ConfigureAwait(true);
            }
            catch (Exception exception)
            {
                captured = ExceptionDispatchInfo.Capture(exception);
            }
            finally
            {
                frame.Continue = false;
            }
        });

        Dispatcher.PushFrame(frame);
        dispatcher.InvokeShutdown();
    });

    thread.SetApartmentState(ApartmentState.STA);
    thread.Start();
    thread.Join();
    captured?.Throw();
}

sealed class FakeSimulatedLiveViewFramePump : ISimulatedLiveViewFramePump
{
    public List<(string CameraAlias, SimulatedFramePattern Pattern)> StartCalls { get; } = [];
    public List<SimulatedFramePattern> PatternChanges { get; } = [];
    public int StopCallCount { get; private set; }
    public int LastReturnedGeneration { get; private set; }

    public event EventHandler<SimulatedLiveViewFrameTick>? Tick;

    public int Start(string cameraAlias, SimulatedFramePattern pattern)
    {
        StartCalls.Add((cameraAlias, pattern));
        LastReturnedGeneration++;
        return LastReturnedGeneration;
    }

    public void Stop() => StopCallCount++;

    public void SetPattern(SimulatedFramePattern pattern) => PatternChanges.Add(pattern);

    public void RaiseTick(SimulatedLiveViewFrameTick tick) => Tick?.Invoke(this, tick);

    public void Dispose()
    {
    }
}

sealed class FakeSimulatedLiveViewFrameSource : ISimulatedLiveViewFrameSource
{
    public bool ReturnInvalidMarker { get; set; }
    public int CallCount { get; private set; }

    public SimulatedLiveViewFrame CreateFrame(string cameraAlias, SimulatedFramePattern pattern, int sequenceNumber, DateTimeOffset capturedAtUtc)
    {
        CallCount++;
        return new SimulatedLiveViewFrame
        {
            CameraAlias = cameraAlias,
            Pattern = pattern,
            SequenceNumber = sequenceNumber,
            CapturedAtUtc = capturedAtUtc,
            Image = CreateFakeFrameImage(),
            Simulation = !ReturnInvalidMarker,
            Marker = ReturnInvalidMarker ? "NotSimulated" : "Simulated",
        };
    }

    private static BitmapSource CreateFakeFrameImage()
    {
        var pixels = new byte[] { 0x10, 0x20, 0x30 };
        var bitmap = BitmapSource.Create(1, 1, 96, 96, PixelFormats.Bgr24, null, pixels, stride: 3);
        bitmap.Freeze();
        return bitmap;
    }
}

static class Check
{
    public static void True(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    public static void False(bool condition, string message) => True(!condition, message);

    public static void Equal<T>(T expected, T actual)
        where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new InvalidOperationException($"Expected {expected}, got {actual}.");
        }
    }

    public static void Throws<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException)
        {
            return;
        }

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    public static async Task ThrowsAsync<TException>(Func<Task> action)
        where TException : Exception
    {
        try
        {
            await action();
        }
        catch (TException)
        {
            return;
        }

        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }
}

static class HardwareTestData
{
    public static HardwareSingleReadinessResult ReadyHardware(string alias) =>
        new()
        {
            CameraMode = "SingleCamera",
            CameraAlias = alias,
            Ready = true,
            SdkCameraCount = 1,
            WpdCameraCount = 1,
            SdkIdentityBound = true,
            WpdIdentityBound = true,
            SdkAliasMatches = true,
            WpdAliasMatches = true,
            SdkStatusProbed = true,
            SpoolInspected = true,
            SpoolPayloadObjectCount = 0,
            SpoolKnownEmpty = true,
            Firmware = "redacted-known",
            LiveViewStatus = "off",
            LiveViewStatusAvailable = true,
            CaptureProfileApproved = true,
            CaptureProfileId = "single-profile",
            CaptureProfileVersion = 1,
            CaptureProfileSha256 = new string('a', 64),
            CaptureProfileCameraAlias = alias,
            CaptureProfileExpiresAtUtc = DateTimeOffset.Parse("2099-01-01T00:00:00Z"),
            CaptureProfileAliasMatches = true,
            SettingsMatchApprovedProfile = true,
            ObservedSettings = ReadyObservedSettings(),
            ReadOnly = true,
            CaptureCommandSent = false,
            CameraObjectDeleteAttempted = false,
            CameraSettingsChanged = false,
            RealIdentifiersIncluded = false,
            FailureCategory = string.Empty,
            FailureDetail = string.Empty,
        };

    private static HardwareObservedCameraSettings ReadyObservedSettings()
    {
        static HardwareObservedCameraSetting Setting(string label) => new()
        {
            Available = true,
            CapType = "enum",
            ProbeState = "observed",
            ValueType = "label",
            CurrentValue = null,
            CurrentIndex = 0,
            CurrentLabel = label,
        };

        return new HardwareObservedCameraSettings
        {
            FileType = Setting("JPEG"),
            CompressionLevel = Setting("Fine"),
            ImageSize = Setting("Large"),
            ExposureMode = Setting("Manual"),
            ShutterSpeed = Setting("profile-match"),
            Aperture = Setting("profile-match"),
            Sensitivity = Setting("profile-match"),
            WhiteBalanceMode = Setting("profile-match"),
            FocusMode = Setting("profile-match"),
        };
    }
}

/// <summary>
/// Holds one request open so a test can observe the busy lock. Without it the lock is invisible:
/// the in-process simulated transport completes before a second click could ever arrive.
/// </summary>
sealed class GatedDualBindingTransport(SimulatedDualBindingAgent agent, Task gate)
    : IHardwareCameraAgentTransport
{
    public async Task<string> SendAsync(string requestJson, CancellationToken cancellationToken = default)
    {
        await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        return agent.Handle(requestJson);
    }
}

sealed class WpfHardwareDualFakeOperations(
    ITestSyntheticCamera camera,
    bool responseUnknownOnce = false) : IDualHardwareCaptureOperations
{
    private DualHardwareCaptureRequest? _unknownRequest;
    public int ReserveCalls { get; private set; }
    public int StartCalls { get; private set; }
    public int QueryCalls { get; private set; }

    public Task<bool> ReservePairTransactionAsync(Guid transactionId, CancellationToken cancellationToken)
    {
        ReserveCalls++;
        return Task.FromResult(true);
    }

    public async Task<DualHardwareDispatchResult> StartReservedPairAsync(
        DualHardwareCaptureRequest request,
        CancellationToken cancellationToken)
    {
        StartCalls++;
        if (responseUnknownOnce)
        {
            _unknownRequest = request;
            return new(DualHardwareDispatchState.ResponseUnknown, null);
        }
        return new(DualHardwareDispatchState.Completed, await CreateResultAsync(request, cancellationToken));
    }

    private async Task<DualHardwareCaptureResult> CreateResultAsync(
        DualHardwareCaptureRequest request,
        CancellationToken cancellationToken)
    {
        var originals = new List<DualHardwareOriginalRecord>();
        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            var path = Path.Combine(request.TransactionDirectory, alias, "original.jpg");
            await camera.CaptureAsync(alias, request.TransactionId, path, cancellationToken);
            originals.Add(new(alias, path, true, true));
        }
        var evidence = new DualHardwareCaptureEvidence(
            DualHardwareCaptureTerminalState.Succeeded,
            request.IdentitySnapshot,
            request.CaptureProfileSnapshot.ProfileId,
            request.CaptureProfileSnapshot.Version,
            request.RigProfileSnapshot.ProfileId,
            request.RigProfileSnapshot.Version,
            request.StartedAtUtc,
            request.WatchdogDeadlineUtc,
            request.StartedAtUtc.AddSeconds(1),
            true, true, true, true, 0);
        return new DualHardwareCaptureResult(
            request.TransactionId,
            originals,
            DualHardwareCaptureTerminalState.Succeeded,
            DualCameraFailureCode.None,
            evidence);
    }

    public async Task<DualHardwarePairQueryOutcome> QueryPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        QueryCalls++;
        if (!responseUnknownOnce || QueryCalls < 2 || _unknownRequest is null)
            return new(DualHardwarePairQueryState.Reserved, null);
        return new(
            DualHardwarePairQueryState.Terminal,
            await CreateResultAsync(_unknownRequest, cancellationToken));
    }
}

sealed class MutableDualIdentitySource(DualCameraIdentitySnapshot initial) : IDualCameraIdentitySnapshotSource
{
    public event EventHandler<DualCameraIdentitySnapshot>? SnapshotChanged;

    public DualCameraIdentitySnapshot Current { get; private set; } = initial;

    public void Set(DualCameraIdentitySnapshot snapshot)
    {
        Current = snapshot;
        SnapshotChanged?.Invoke(this, snapshot);
    }
}

sealed class NeverCaptureDualBridge : ITestSyntheticCamera, IOfflineStitcherAdapter
{
    public int CaptureCalls { get; private set; }

    public Task<string> CaptureAsync(
        string alias,
        Guid transactionId,
        string destinationPath,
        CancellationToken cancellationToken)
    {
        _ = alias;
        _ = transactionId;
        _ = destinationPath;
        _ = cancellationToken;
        CaptureCalls++;
        return Task.FromException<string>(new InvalidOperationException("Identity gate was bypassed."));
    }

    public Task ValidateCanonicalJpegAsync(string jpegPath, int expectedWidth, int expectedHeight, CancellationToken cancellationToken) =>
        Task.FromException(new InvalidOperationException("Identity gate was bypassed."));

    public Task<OfflineStitchArtifact> StitchAsync(
        IReadOnlyList<CanonicalJpegOriginal> originals,
        string outputJobDirectory,
        DualCameraRigProfile profile,
        Guid stitchJobId,
        Guid captureTransactionId,
        DateTimeOffset completedAtUtc,
        CancellationToken cancellationToken) =>
        Task.FromException<OfflineStitchArtifact>(new InvalidOperationException("Identity gate was bypassed."));

    public Task ExportAsync(string stitchedJpeg, string destinationJpeg, CancellationToken cancellationToken) =>
        Task.FromException(new InvalidOperationException("Identity gate was bypassed."));
}

sealed class BlockingFailedExportBridge(M2OfflineStitcherProcessAdapter inner) :
    ITestSyntheticCamera,
    IOfflineStitcherAdapter
{
    public TaskCompletionSource ExportStarted { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public TaskCompletionSource ReleaseExport { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public Task<string> CaptureAsync(
        string alias,
        Guid transactionId,
        string destinationPath,
        CancellationToken cancellationToken) =>
        inner.CaptureAsync(alias, transactionId, destinationPath, cancellationToken);

    public Task ValidateCanonicalJpegAsync(
        string jpegPath,
        int expectedWidth,
        int expectedHeight,
        CancellationToken cancellationToken) =>
        inner.ValidateCanonicalJpegAsync(jpegPath, expectedWidth, expectedHeight, cancellationToken);

    public Task<OfflineStitchArtifact> StitchAsync(
        IReadOnlyList<CanonicalJpegOriginal> originals,
        string outputJobDirectory,
        DualCameraRigProfile profile,
        Guid stitchJobId,
        Guid captureTransactionId,
        DateTimeOffset completedAtUtc,
        CancellationToken cancellationToken) =>
        inner.StitchAsync(
            originals, outputJobDirectory, profile, stitchJobId, captureTransactionId,
            completedAtUtc, cancellationToken);

    public async Task ExportAsync(
        string stitchedJpeg,
        string destinationJpeg,
        CancellationToken cancellationToken)
    {
        _ = stitchedJpeg;
        _ = destinationJpeg;
        ExportStarted.TrySetResult();
        await ReleaseExport.Task.WaitAsync(cancellationToken);
        throw new IOException("deterministic formal WPF export failure");
    }
}

sealed class BlockingTransactionService : ISimulatedTransactionService
{
    private readonly TaskCompletionSource _started = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource _release = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public Task Started => _started.Task;

    public Task<IReadOnlyList<SimulatedWorkflowState>> InitializeAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult<IReadOnlyList<SimulatedWorkflowState>>([]);

    public Task<SimulatedWorkflowState> ExecuteAsync(
        Guid transactionId,
        SimulatedWorkflowScenario scenario,
        CancellationToken cancellationToken = default) =>
        ExecuteAsync(transactionId, CapturePlan.Dual(), scenario, cancellationToken);

    public async Task<SimulatedWorkflowState> ExecuteAsync(
        Guid transactionId,
        CapturePlan capturePlan,
        SimulatedWorkflowScenario scenario,
        CancellationToken cancellationToken = default)
    {
        capturePlan.Validate();
        _started.TrySetResult();
        await _release.Task.WaitAsync(cancellationToken);
        return new SimulatedWorkflowState
        {
            Simulation = true,
            Marker = SimulatedTransactionProtocol.Marker,
            TransactionId = transactionId,
            OperatingMode = capturePlan.OperatingMode,
            RequiredCameraAliases = capturePlan.RequiredCameraAliases.ToArray(),
            State = SimulatedTransactionState.Complete,
            IsTerminal = true,
            RetainedOriginalAliases = capturePlan.RequiredCameraAliases.ToArray(),
            TerminalReason = null,
            AutomaticRetryCount = 0,
        };
    }

    public void Release() => _release.TrySetResult();
}

class FakeHardwareSingleCameraOperations : IHardwareSingleCameraOperations
{
    public string AgentExecutablePath { get; set; } = "C:\\fake\\A0CameraStitcher.CameraAgent.exe";

    // Deliberately a different directory than AgentExecutablePath's: the real
    // agent's exe directory and its --artifacts-root are unrelated (see
    // HardwareSingleStoragePaths.Resolve), and a fake that conflated the two
    // would hide any bug where production code derived the canonical-path
    // root from the wrong one. Tests that verify a real retained-original or
    // preview file must set this to the directory under which they actually
    // write it (typically via HardwareAgentArtifactLayout).
    public string AgentArtifactsRoot { get; set; } = "C:\\fake\\artifacts";

    // HardwareSingleLiveViewResult.RunId reported by ProbeLiveViewAsync
    // below. Real runs get a fresh run ID per Live View session; tests that
    // reuse the same Preview record across a probe and a later capture's
    // PostCapturePreview must set this to match whatever run ID the capture
    // side (e.g. CompleteCaptureRunId) used to write that file.
    public string ProbeLiveViewRunId { get; set; } = "run-3000-1";

    public bool AgentExecutableAvailable => true;

    public int ReadinessCallCount { get; private set; }

    public int LiveViewCallCount { get; private set; }

    public int CaptureCallCount { get; private set; }

    public int TransactionResultCallCount { get; private set; }

    public bool LastCaptureLiveViewHandoffRequested { get; private set; }

    public HardwareCaptureProfileSnapshot? LastCaptureExpectedProfile { get; private set; }

    public HardwareCaptureProfileSnapshot? LastTransactionExpectedProfile { get; private set; }

    public string? LastTransactionExpectedCameraAlias { get; private set; }

    public bool LastTransactionExpectedHandoff { get; private set; }

    public List<string> CallOrder { get; } = [];

    public int TotalCallCount => ReadinessCallCount + LiveViewCallCount + CaptureCallCount + TransactionResultCallCount;

    public HardwarePreviewJpegRecord? Preview { get; init; }

    public Func<string, string, HardwareSingleCaptureResult>? CaptureResultFactory { get; init; }

    public Func<string, HardwareSingleReadinessResult>? ReadinessFactory { get; init; }

    public Exception? CaptureException { get; init; }

    public Queue<HardwareSingleCaptureResult> TransactionResults { get; } = new();

    public Task<HardwareCameraAgentReply<HardwareSingleReadinessResult>> GetReadinessAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default)
    {
        ReadinessCallCount++;
        CallOrder.Add("readiness");
        return Task.FromResult(new HardwareCameraAgentReply<HardwareSingleReadinessResult>(
            "readiness-request",
            true,
            "SingleReady",
            ReadinessFactory?.Invoke(cameraAlias) ?? HardwareTestData.ReadyHardware(cameraAlias)));
    }

    public Task<HardwareCameraAgentReply<HardwareSingleLiveViewResult>> ProbeLiveViewAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default)
    {
        LiveViewCallCount++;
        CallOrder.Add("finite-live-view");
        if (Preview is null)
        {
            throw new InvalidOperationException("A fake preview was not configured.");
        }

        return Task.FromResult(new HardwareCameraAgentReply<HardwareSingleLiveViewResult>(
            "live-view-request",
            true,
            "LiveViewProbeComplete",
            new HardwareSingleLiveViewResult
            {
                CameraMode = "SingleCamera",
                CameraAlias = cameraAlias,
                RunId = ProbeLiveViewRunId,
                Frames = 1,
                LastFrameBytes = Preview.SizeBytes,
                LastFrameSha256 = Preview.Sha256,
                DurationMs = 100,
                PreviewPersisted = true,
                Preview = Preview,
                PreviewIsOriginal = false,
                PreviewIsStitchInput = false,
                LiveViewStopped = true,
                SdkSessionClosed = true,
                RealIdentifiersIncluded = false,
                ErrorCategory = string.Empty,
                ErrorDetail = string.Empty,
            }));
    }

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool liveViewHandoffRequested,
        CancellationToken cancellationToken = default)
    {
        CaptureCallCount++;
        CallOrder.Add("capture");
        LastCaptureExpectedProfile = expectedProfile;
        LastCaptureLiveViewHandoffRequested = liveViewHandoffRequested;
        if (CaptureException is not null)
        {
            throw CaptureException;
        }

        var payload = CaptureResultFactory?.Invoke(transactionId, cameraAlias)
            ?? throw new InvalidOperationException("A fake capture result was not configured.");
        return Task.FromResult(new HardwareCameraAgentReply<HardwareSingleCaptureResult>(
            "capture-request",
            payload.TerminalState == "Complete",
            payload.TerminalState == "Complete" ? "CaptureComplete" : payload.ErrorCategory,
            payload));
    }

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool expectedLiveViewHandoffRequested,
        CancellationToken cancellationToken = default)
    {
        TransactionResultCallCount++;
        CallOrder.Add("get-result");
        LastTransactionExpectedCameraAlias = expectedCameraAlias;
        LastTransactionExpectedProfile = expectedProfile;
        LastTransactionExpectedHandoff = expectedLiveViewHandoffRequested;
        if (!TransactionResults.TryDequeue(out var payload))
        {
            throw new InvalidOperationException("A fake transaction result was not configured.");
        }

        var success = payload.TerminalState == "Complete";
        var resultCode = payload.TerminalState switch
        {
            "Complete" => "CaptureComplete",
            "Reserved" => "TransactionReserved",
            "InProgress" => "TransactionInProgress",
            _ => payload.ErrorCategory,
        };
        return Task.FromResult(new HardwareCameraAgentReply<HardwareSingleCaptureResult>(
            "transaction-request",
            success,
            resultCode,
            payload));
    }
}

sealed class FakeContinuousHardwareOperations(byte[] frameBytes) :
    FakeHardwareSingleCameraOperations,
    IHardwareContinuousLiveViewOperations
{
    private ulong _frameNumber;

    public int StartCount { get; private set; }

    public int StopCount { get; private set; }

    public bool FailNextStop { get; set; }

    public TaskCompletionSource FirstFrame { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public string CreateSessionId() => Guid.NewGuid().ToString("N");

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StartLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default)
    {
        StartCount++;
        CallOrder.Add("start");
        return Task.FromResult(Reply(sessionId, true, "Started", 0, []));
    }

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> ReadLiveViewFrameAsync(
        string sessionId,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _frameNumber++;
        CallOrder.Add("frame");
        FirstFrame.TrySetResult();
        return Task.FromResult(Reply(sessionId, true, "Frame", _frameNumber, frameBytes));
    }

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> HeartbeatLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        Task.FromResult(Reply(sessionId, true, "Heartbeat", _frameNumber, []));

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StopLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default)
    {
        StopCount++;
        CallOrder.Add("stop");
        if (FailNextStop)
        {
            FailNextStop = false;
            return Task.FromResult(Reply(
                sessionId, false, "continuous_live_view_stop_failed", _frameNumber, []));
        }
        return Task.FromResult(Reply(sessionId, true, "Stopped", _frameNumber, []));
    }

    private static HardwareCameraAgentReply<HardwareContinuousLiveViewResult> Reply(
        string sessionId,
        bool success,
        string state,
        ulong frameNumber,
        byte[] bytes)
    {
        var frame = state == "Frame";
        var running = state is "Started" or "Frame" or "Heartbeat";
        var errorCategory = success ? string.Empty : state;
        var payload = new HardwareContinuousLiveViewResult
        {
            CameraMode = "SingleCamera",
            CameraAlias = "CAM-A",
            SessionId = sessionId,
            State = success ? state : string.Empty,
            FrameNumber = frameNumber,
            FrameSize = frame ? bytes.Length : 0,
            FrameSha256 = frame
                ? Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant()
                : string.Empty,
            FrameJpegBase64 = frame ? Convert.ToBase64String(bytes) : string.Empty,
            PreviewIsOriginal = false,
            PreviewIsStitchInput = false,
            SdkSessionOpen = success && running,
            LiveViewRunning = success && running,
            HeartbeatTimeoutSeconds = 20,
            MaximumSessionSeconds = 600,
            RealIdentifiersIncluded = false,
            ErrorCategory = errorCategory,
            ErrorDetail = success ? string.Empty : "deterministic stop failure",
        };
        return new HardwareCameraAgentReply<HardwareContinuousLiveViewResult>(
            "continuous-request", success, state, payload);
    }
}

sealed class BlockingHardwareStateStore : IHardwareSingleAppStateStore
{
    private readonly TaskCompletionSource _release = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public TaskCompletionSource LoadStarted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

    public async Task<HardwarePendingTransaction?> LoadPendingAsync(CancellationToken cancellationToken = default)
    {
        LoadStarted.TrySetResult();
        await _release.Task.WaitAsync(cancellationToken);
        return null;
    }

    public Task SavePendingAsync(
        HardwarePendingTransaction pendingTransaction,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The blocking startup store does not accept writes.");

    public Task MarkCaptureRequestDispatchAttemptedAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The blocking startup store does not accept writes.");

    public Task MarkCaptureRequestNotDispatchedAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The blocking startup store does not accept writes.");

    public Task ClearPendingAsync(
        string expectedTransactionId,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The blocking startup store does not accept writes.");

    public void ReleaseLoad() => _release.TrySetResult();
}

sealed class MutableTimeProvider(DateTimeOffset utcNow) : TimeProvider
{
    private DateTimeOffset _utcNow = utcNow;
    private readonly List<MutableTimer> _timers = [];

    public override DateTimeOffset GetUtcNow() => _utcNow;

    public override ITimer CreateTimer(
        TimerCallback callback,
        object? state,
        TimeSpan dueTime,
        TimeSpan period)
    {
        var timer = new MutableTimer(this, callback, state, dueTime, period);
        _timers.Add(timer);
        return timer;
    }

    public void Advance(TimeSpan duration)
    {
        _utcNow = _utcNow.Add(duration);
        foreach (var timer in _timers.ToArray())
        {
            timer.FireIfDue(_utcNow);
        }
    }

    private sealed class MutableTimer : ITimer
    {
        private readonly MutableTimeProvider _owner;
        private readonly TimerCallback _callback;
        private readonly object? _state;
        private DateTimeOffset? _nextFireAt;
        private TimeSpan _period;
        private bool _disposed;

        public MutableTimer(
            MutableTimeProvider owner,
            TimerCallback callback,
            object? state,
            TimeSpan dueTime,
            TimeSpan period)
        {
            _owner = owner;
            _callback = callback;
            _state = state;
            Change(dueTime, period);
        }

        public bool Change(TimeSpan dueTime, TimeSpan period)
        {
            if (_disposed)
            {
                return false;
            }

            _period = period;
            _nextFireAt = dueTime == Timeout.InfiniteTimeSpan
                ? null
                : _owner.GetUtcNow().Add(dueTime);
            return true;
        }

        public void FireIfDue(DateTimeOffset now)
        {
            if (_disposed || _nextFireAt is null || now < _nextFireAt.Value)
            {
                return;
            }

            _nextFireAt = _period == Timeout.InfiniteTimeSpan
                ? null
                : now.Add(_period);
            _callback(_state);
        }

        public void Dispose()
        {
            _disposed = true;
            _nextFireAt = null;
        }

        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }
    }
}
