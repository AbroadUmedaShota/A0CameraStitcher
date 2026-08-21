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
    private readonly Func<string> _requestIdFactory;
    private readonly Func<DateTimeOffset> _utcNow;

    public DualBindingSessionClient(
        IHardwareCameraAgentTransport transport,
        Func<string>? requestIdFactory = null,
        Func<DateTimeOffset>? utcNow = null)
    {
        ArgumentNullException.ThrowIfNull(transport);
        _transport = transport;
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
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingLiveViewResult>.Refused(noSession);
        }

        var requestId = _requestIdFactory();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeStartLiveViewResponse(
            await _transport.SendAsync(
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
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingFrameResult>.Refused(noSession);
        }

        var requestId = _requestIdFactory();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeFrameResponse(
            await _transport.SendAsync(
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
            await _transport.SendAsync(
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
        if (RequireSession() is { } noSession)
        {
            return DualBindingReply<DualBindingCompleteResult>.Refused(noSession);
        }

        var requestId = _requestIdFactory();
        var confirmedAtUtc = _utcNow();
        var reply = DualBindingCameraAgentProtocolCodec.DeserializeCompleteBindingResponse(
            await _transport.SendAsync(
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
        if (RequireSession() is { } noSession)
        {
            return noSession;
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

    private DualBindingRefusal? RequireSession() =>
        SessionId.Length == 0
            ? new DualBindingRefusal
            {
                ResultCode = "NoBindingSession",
                Detail = "No binding session has been started.",
            }
            : null;

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
        DiscardSessionArtifacts();
    }
}
