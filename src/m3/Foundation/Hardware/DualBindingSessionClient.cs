namespace A0CameraStitcher.M3.Foundation.Hardware;

/// <summary>
/// Typed client and session lifecycle for <c>a0.camera-agent.hardware-dual-binding.v1</c>
/// (ADR-0025, Issue #62).
/// </summary>
/// <remarks>
/// <para>
/// The binding lives in the memory of the one Agent process that produced the candidates. This
/// client therefore holds nothing durable either: no session id on disk, no candidate ordinal, no
/// preview, no source object. Everything it knows dies with the instance, which is the same
/// lifetime the binding itself has.
/// </para>
/// <para>
/// Every operation that needs a session refuses locally when there is none, before touching the
/// wire. That is not an optimisation: sending an operation with no session would make the agent
/// answer <c>SessionMismatch</c>, which reads to the caller exactly like an invalidation and would
/// send the operator to a "re-bind" screen when the truth is "nothing was ever started".
/// </para>
/// </remarks>
public sealed class DualBindingSessionClient
{
    private readonly IHardwareCameraAgentTransport _transport;
    private readonly IHardwareCameraAgentProcessLifetime? _processLifetime;
    private readonly IHardwareCameraAgentGenerationBoundTransport? _generationBoundTransport;
    private readonly Func<string> _requestIdFactory;
    private readonly Func<DateTimeOffset> _utcNow;
    private bool _captureHostActivated;
    private long? _bindingProcessGeneration;
    private long? _captureHostProcessGeneration;

    public DualBindingSessionClient(
        IHardwareCameraAgentTransport transport,
        Func<string>? requestIdFactory = null,
        Func<DateTimeOffset>? utcNow = null)
    {
        ArgumentNullException.ThrowIfNull(transport);
        _transport = transport;
        _processLifetime = transport as IHardwareCameraAgentProcessLifetime;
        _generationBoundTransport = transport as IHardwareCameraAgentGenerationBoundTransport;
        _requestIdFactory = requestIdFactory ?? (() => $"binding-{Guid.NewGuid():N}");
        _utcNow = utcNow ?? (() => DateTimeOffset.UtcNow);
    }

    /// <summary>Empty until a binding session starts, and cleared again the moment one ends.</summary>
    public string SessionId { get; private set; } = string.Empty;

    public DualBindingSessionState State { get; private set; } = DualBindingSessionState.None;

    public DualBindingInvalidationReason InvalidationReason { get; private set; } =
        DualBindingInvalidationReason.None;

    public IReadOnlyList<int> CandidateOrdinals { get; private set; } = Array.Empty<int>();

    /// <summary>Ordinal to alias, as the operator confirmed them. Empty until they do.</summary>
    public IReadOnlyDictionary<int, string> Assignments => _assignments;

    public IReadOnlyList<DualBindingEvidence> Evidence { get; private set; } =
        Array.Empty<DualBindingEvidence>();

    /// <summary>
    /// Capture may only start when this is true. There is no other gate and no Single-camera
    /// fallback: a Dual capture with an unconfirmed binding would attribute one body's frame to the
    /// other alias, which is undetectable afterwards.
    /// </summary>
    public bool IsReady => State == DualBindingSessionState.Ready;

    /// <summary>
    /// The binding stopped being trustworthy and only a fresh session can recover. The operator has
    /// to be told why, so <see cref="InvalidationReason"/> is kept alongside.
    /// </summary>
    public bool RequiresRebinding => State == DualBindingSessionState.Invalid;

    /// <summary>
    /// True only after the native binding host acknowledged the one-way transition
    /// to its capture pipe. The binding pipe is retired at that point, so neither
    /// cancellation nor a freshness probe may be sent through it again.
    /// </summary>
    public bool CaptureHostActivated =>
        _captureHostActivated &&
        (_processLifetime is null ||
            _captureHostProcessGeneration is { } generation &&
            _processLifetime.IsProcessGenerationAlive(generation));

    private readonly Dictionary<int, string> _assignments = [];

    public async Task<DualBindingReply<DualBindingBeginResult>> BeginBindingAsync(
        CancellationToken cancellationToken = default)
    {
        // Discard the previous session before the request goes out, so a refused begin-binding
        // cannot leave the UI addressing a session that no longer exists.
        ResetSession();

        var requestId = _requestIdFactory();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeBeginBindingResponse(
            await _transport.SendAsync(
                DualBindingCameraAgentProtocolCodec.CreateBeginBindingRequest(requestId),
                cancellationToken).ConfigureAwait(false),
            requestId);

        if (reply.Value is { } started)
        {
            SessionId = started.SessionId;
            State = started.State;
            CandidateOrdinals = started.CandidateOrdinals;
            _bindingProcessGeneration = _processLifetime?.CurrentProcessGeneration;
        }
        else
        {
            ApplyRefusal(reply.Refusal!);
        }

        return reply;
    }

    public async Task<DualBindingReply<DualBindingLiveViewResult>> StartCandidateLiveViewAsync(
        int candidateOrdinal,
        CancellationToken cancellationToken = default)
    {
        if (DetectExitedBindingHost() is { } processExited)
        {
            return DualBindingReply<DualBindingLiveViewResult>.Refused(processExited);
        }
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingLiveViewResult>.Refused(noSession);
        }

        var requestId = _requestIdFactory();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeStartLiveViewResponse(
            await SendSessionRequestAsync(
                DualBindingCameraAgentProtocolCodec.CreateStartLiveViewRequest(
                    requestId, SessionId, candidateOrdinal),
                cancellationToken).ConfigureAwait(false),
            requestId,
            SessionId);

        if (reply.Value is not null)
        {
            ActiveLiveViewOrdinal = candidateOrdinal;
        }
        else
        {
            ActiveLiveViewOrdinal = null;
            ApplyRefusal(reply.Refusal!);
        }

        return reply;
    }

    /// <summary>
    /// The candidate whose Live View is running, or null. Exactly one at a time, ever: the agent
    /// enforces it and this mirrors it so the UI can never render two previews.
    /// </summary>
    public int? ActiveLiveViewOrdinal { get; private set; }

    public async Task<DualBindingReply<DualBindingFrameResult>> GetCandidateLiveViewFrameAsync(
        int candidateOrdinal,
        CancellationToken cancellationToken = default)
    {
        if (DetectExitedBindingHost() is { } processExited)
        {
            return DualBindingReply<DualBindingFrameResult>.Refused(processExited);
        }
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingFrameResult>.Refused(noSession);
        }

        var requestId = _requestIdFactory();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeFrameResponse(
            await SendSessionRequestAsync(
                DualBindingCameraAgentProtocolCodec.CreateFrameRequest(
                    requestId, SessionId, candidateOrdinal),
                cancellationToken).ConfigureAwait(false),
            requestId,
            SessionId);

        if (reply.Value is null)
        {
            ApplyRefusal(reply.Refusal!);
        }

        return reply;
    }

    public async Task<DualBindingReply<DualBindingAliasResult>> ConfirmAliasAsync(
        int candidateOrdinal,
        string cameraAlias,
        CancellationToken cancellationToken = default)
    {
        if (DetectExitedBindingHost() is { } processExited)
        {
            return DualBindingReply<DualBindingAliasResult>.Refused(processExited);
        }
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingAliasResult>.Refused(noSession);
        }

        // Refused locally so the operator's one chance to assign this candidate is not spent on a
        // request the agent would refuse anyway. Re-assigning is not allowed by design: an operator
        // who assigned the same body twice has misread the Live View.
        if (_assignments.ContainsKey(candidateOrdinal))
        {
            return DualBindingReply<DualBindingAliasResult>.Refused(new DualBindingRefusal
            {
                ResultCode = "CandidateAlreadyAssigned",
                SessionId = SessionId,
                State = State,
                Detail = "This candidate already has an alias.",
            });
        }

        if (_assignments.ContainsValue(cameraAlias))
        {
            return DualBindingReply<DualBindingAliasResult>.Refused(new DualBindingRefusal
            {
                ResultCode = "AliasAlreadyAssigned",
                SessionId = SessionId,
                State = State,
                Detail = $"{cameraAlias} is already assigned to another candidate.",
            });
        }

        var requestId = _requestIdFactory();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeConfirmAliasResponse(
            await SendSessionRequestAsync(
                DualBindingCameraAgentProtocolCodec.CreateConfirmAliasRequest(
                    requestId, SessionId, candidateOrdinal, cameraAlias),
                cancellationToken).ConfigureAwait(false),
            requestId,
            SessionId,
            cameraAlias);

        if (reply.Value is { } confirmed)
        {
            _assignments[confirmed.CandidateOrdinal] = confirmed.CameraAlias;
            State = confirmed.State;
            // The assignment ended this candidate's Live View and closed its SDK session.
            ActiveLiveViewOrdinal = null;
        }
        else
        {
            ApplyRefusal(reply.Refusal!);
        }

        return reply;
    }

    public async Task<DualBindingReply<DualBindingCompleteResult>> CompleteBindingAsync(
        CancellationToken cancellationToken = default)
    {
        if (DetectExitedBindingHost() is { } processExited)
        {
            return DualBindingReply<DualBindingCompleteResult>.Refused(processExited);
        }
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingCompleteResult>.Refused(noSession);
        }

        var requestId = _requestIdFactory();
        var confirmedAtUtc = _utcNow();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeCompleteBindingResponse(
            await SendSessionRequestAsync(
                DualBindingCameraAgentProtocolCodec.CreateCompleteBindingRequest(
                    requestId,
                    SessionId,
                    new DateTimeOffset(confirmedAtUtc.UtcDateTime, TimeSpan.Zero)),
                cancellationToken).ConfigureAwait(false),
            requestId,
            SessionId);

        if (reply.Value is { } completed)
        {
            State = completed.State;
            Evidence = completed.Evidence;
        }
        else
        {
            ApplyRefusal(reply.Refusal!);
        }

        return reply;
    }

    /// <summary>
    /// Performs the final freshness-checked handoff from the binding pipe to the
    /// capture pipe. This is deliberately separate from complete-binding so a
    /// Ready session can still be rechecked or safely cancelled while no capture
    /// has been dispatched.
    /// </summary>
    public async Task<DualBindingReply<DualBindingCaptureActivationResult>> ActivateCaptureAsync(
        CancellationToken cancellationToken = default)
    {
        if (DetectExitedBindingHost() is { } bindingHostExited)
        {
            return DualBindingReply<DualBindingCaptureActivationResult>.Refused(bindingHostExited);
        }
        if (DetectExitedCaptureHost() is { } processExited)
        {
            return DualBindingReply<DualBindingCaptureActivationResult>.Refused(processExited);
        }
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingCaptureActivationResult>.Refused(noSession);
        }
        if (!IsReady)
        {
            return DualBindingReply<DualBindingCaptureActivationResult>.Refused(new DualBindingRefusal
            {
                ResultCode = "BindingNotReady",
                SessionId = SessionId,
                State = State,
                Detail = "The binding must be Ready before capture activation.",
            });
        }
        if (CaptureHostActivated)
        {
            return DualBindingReply<DualBindingCaptureActivationResult>.Refused(new DualBindingRefusal
            {
                ResultCode = "CaptureAlreadyActivated",
                SessionId = SessionId,
                State = State,
                Detail = "The binding pipe has already transitioned to capture.",
            });
        }

        var requestId = _requestIdFactory();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeActivateCaptureResponse(
            await SendSessionRequestAsync(
                DualBindingCameraAgentProtocolCodec.CreateActivateCaptureRequest(requestId, SessionId),
                cancellationToken).ConfigureAwait(false),
            requestId,
            SessionId);
        if (reply.Value is not null)
        {
            _captureHostActivated = true;
            _captureHostProcessGeneration = _processLifetime?.CurrentProcessGeneration;
            if (DetectExitedCaptureHost() is { } exitedAfterActivation)
            {
                return DualBindingReply<DualBindingCaptureActivationResult>.Refused(exitedAfterActivation);
            }
        }
        else
        {
            ApplyRefusal(reply.Refusal!);
        }

        return reply;
    }

    /// <summary>
    /// Ends the active binding session and confirms that Live View, candidate
    /// source, SDK manager and process-side session were all released. It is a
    /// single attempt and is never sent after capture activation.
    /// </summary>
    public async Task<DualBindingReply<DualBindingCancellationResult>> CancelBindingAsync(
        CancellationToken cancellationToken = default)
    {
        if (DetectExitedBindingHost() is { } bindingHostExited)
        {
            return DualBindingReply<DualBindingCancellationResult>.Refused(bindingHostExited);
        }
        if (DetectExitedCaptureHost() is { } processExited)
        {
            return DualBindingReply<DualBindingCancellationResult>.Refused(processExited);
        }
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingCancellationResult>.Refused(noSession);
        }
        if (CaptureHostActivated)
        {
            return DualBindingReply<DualBindingCancellationResult>.Refused(new DualBindingRefusal
            {
                ResultCode = "CaptureAlreadyActivated",
                SessionId = SessionId,
                State = State,
                Detail = "The binding pipe has already transitioned to capture.",
            });
        }

        var requestId = _requestIdFactory();
        var sessionId = SessionId;
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeCancelBindingResponse(
            await SendSessionRequestAsync(
                DualBindingCameraAgentProtocolCodec.CreateCancelBindingRequest(requestId, sessionId),
                cancellationToken).ConfigureAwait(false),
            requestId,
            sessionId);
        if (reply.Value is not null)
        {
            ResetSession();
        }
        else
        {
            ApplyRefusal(reply.Refusal!);
        }

        return reply;
    }

    /// <summary>
    /// Asks the agent whether this binding is still the one it is serving, without changing
    /// anything.
    /// </summary>
    /// <remarks>
    /// The protocol is request/response with no server push, so a Ready binding cannot learn that
    /// it was invalidated until something asks. Nothing does, between confirming the binding and
    /// starting a capture -- which is exactly the window in which a body can be unplugged.
    /// <para>
    /// The probe re-issues <c>complete-binding</c>, which on a Ready session is refused with
    /// <c>BindingAlreadyComplete</c> and changes no state, but which still runs the agent's session
    /// identity check and invalidation poll first. So a pending invalidation surfaces here, before
    /// a capture is dispatched against a binding that stopped being trustworthy.
    /// </para>
    /// </remarks>
    public async Task<DualBindingRefusal?> VerifyBindingIsCurrentAsync(
        CancellationToken cancellationToken = default)
    {
        if (DetectExitedBindingHost() is { } bindingHostExited)
        {
            return bindingHostExited;
        }
        if (DetectExitedCaptureHost() is { } processExited)
        {
            return processExited;
        }
        if (RequireSession() is { } noSession)
        {
            return noSession;
        }
        if (CaptureHostActivated)
        {
            return new DualBindingRefusal
            {
                ResultCode = "CaptureAlreadyActivated",
                SessionId = SessionId,
                State = State,
                Detail = "The binding pipe has already transitioned to capture.",
            };
        }

        var reply = await CompleteBindingAsync(cancellationToken).ConfigureAwait(false);
        if (reply.Refusal is { ResultCode: "BindingAlreadyComplete" })
        {
            // Still Ready. CompleteBindingAsync's refusal handling leaves State untouched for this
            // code, so nothing needs restoring.
            return null;
        }

        return reply.Refusal;
    }

    /// <summary>
    /// Applies a capture-pipe invalidation locally after activation. The binding pipe was retired
    /// by activation, so this deliberately sends no protocol operation.
    /// </summary>
    public void InvalidateActivatedCaptureBinding(DualBindingInvalidationReason reason)
    {
        if (reason == DualBindingInvalidationReason.None)
            throw new ArgumentOutOfRangeException(nameof(reason));

        ResetSession();
        State = DualBindingSessionState.Invalid;
        InvalidationReason = reason;
    }

    private DualBindingRefusal? RequireSession() =>
        SessionId.Length == 0
            ? new DualBindingRefusal
            {
                ResultCode = "NoBindingSession",
                Detail = "No binding session has been started.",
            }
            : null;

    private Task<string> SendSessionRequestAsync(
        string requestJson,
        CancellationToken cancellationToken)
    {
        if (_generationBoundTransport is not null && _bindingProcessGeneration is { } generation)
        {
            return _generationBoundTransport.SendAsync(requestJson, generation, cancellationToken);
        }

        return _transport.SendAsync(requestJson, cancellationToken);
    }

    private DualBindingRefusal? DetectExitedBindingHost()
    {
        if (_captureHostActivated ||
            SessionId.Length == 0 ||
            _processLifetime is null ||
            _bindingProcessGeneration is not { } generation ||
            _processLifetime.IsProcessGenerationAlive(generation))
        {
            return null;
        }

        var expiredSessionId = SessionId;
        ResetSession();
        State = DualBindingSessionState.Invalid;
        InvalidationReason = DualBindingInvalidationReason.AgentRestart;
        return new DualBindingRefusal
        {
            ResultCode = "BindingHostExpired",
            SessionId = expiredSessionId,
            State = DualBindingSessionState.Invalid,
            InvalidationReason = DualBindingInvalidationReason.AgentRestart,
            Detail = "The Camera Agent process that created this binding expired; no replacement Agent was started.",
        };
    }

    private DualBindingRefusal? DetectExitedCaptureHost()
    {
        if (!_captureHostActivated || CaptureHostActivated)
        {
            return null;
        }

        var expiredSessionId = SessionId;
        ResetSession();
        State = DualBindingSessionState.Invalid;
        InvalidationReason = DualBindingInvalidationReason.AgentRestart;
        return new DualBindingRefusal
        {
            ResultCode = "SessionMismatch",
            SessionId = expiredSessionId,
            State = DualBindingSessionState.Invalid,
            InvalidationReason = DualBindingInvalidationReason.AgentRestart,
            Detail = "The activated Camera Agent process exited; a fresh binding is required.",
        };
    }

    private void ApplyRefusal(DualBindingRefusal refusal)
    {
        switch (refusal.ResultCode)
        {
            case "BindingInvalidated":
                State = DualBindingSessionState.Invalid;
                if (InvalidationReason == DualBindingInvalidationReason.None)
                {
                    InvalidationReason = refusal.InvalidationReason;
                }

                // Nothing from the invalidated session may be reused -- not the preview, not the
                // ordinals, not the assignments. Re-binding starts from enumeration.
                DiscardSessionArtifacts();
                break;

            case "BindingHostExpired":
            case "SessionMismatch":
                // The agent is not serving this session at all: it restarted, or a newer session
                // replaced it. Either way the id in hand is worthless.
                ResetSession();
                State = DualBindingSessionState.Invalid;
                if (InvalidationReason == DualBindingInvalidationReason.None)
                {
                    InvalidationReason = DualBindingInvalidationReason.AgentRestart;
                }

                break;

            case "QuiesceIncomplete":
            case "CandidateNotQuiesced":
                // A body that would not stop streaming or would not close cannot be part of a
                // trustworthy binding, and the assignment already happened on the agent side, so
                // this session can never reach Ready.
                State = refusal.State == DualBindingSessionState.None ? State : refusal.State;
                DiscardSessionArtifacts();
                break;

            case "BindingCleanupFailed":
                ResetSession();
                State = DualBindingSessionState.Invalid;
                InvalidationReason = refusal.InvalidationReason == DualBindingInvalidationReason.None
                    ? DualBindingInvalidationReason.SdkError
                    : refusal.InvalidationReason;
                break;

            default:
                // Everything else -- an unknown ordinal, a duplicate alias, a Live View that would
                // not start -- leaves the session usable. The operator retries the step.
                if (refusal.State != DualBindingSessionState.None)
                {
                    State = refusal.State;
                }

                break;
        }
    }

    private void DiscardSessionArtifacts()
    {
        _assignments.Clear();
        ActiveLiveViewOrdinal = null;
        Evidence = Array.Empty<DualBindingEvidence>();
    }

    private void ResetSession()
    {
        SessionId = string.Empty;
        State = DualBindingSessionState.None;
        InvalidationReason = DualBindingInvalidationReason.None;
        CandidateOrdinals = Array.Empty<int>();
        _captureHostActivated = false;
        _bindingProcessGeneration = null;
        _captureHostProcessGeneration = null;
        DiscardSessionArtifacts();
    }
}
