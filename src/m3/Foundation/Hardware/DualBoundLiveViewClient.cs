namespace A0CameraStitcher.M3.Foundation.Hardware;

/// <summary>What this client knows about the Agent's bound Live View.</summary>
public enum DualBoundLiveViewState
{
    /// <summary>Nothing has been opened, or a stop was confirmed (Live View stopped and source closed).</summary>
    ConfirmedClosed,

    /// <summary>One camera's Live View is open and was confirmed by the Agent.</summary>
    Live,

    /// <summary>
    /// A request that may have opened or closed something was sent and its outcome is not known to
    /// be "closed": a start whose answer was lost, a stop that was refused or did not confirm both
    /// halves. Only a confirmed stop leaves this state.
    /// </summary>
    MaybeOpen,

    /// <summary>
    /// The binding was invalidated or its host is gone. There is nothing left to stop, and nothing
    /// can be opened until a fresh binding.
    /// </summary>
    Invalidated,
}

/// <param name="Confirmed">The Agent confirmed the Live View stopped and the SDK source closed, or nothing was ever open.</param>
/// <param name="RequestSent">A stop request went on the wire. False when the state was already ConfirmedClosed.</param>
/// <param name="Refusal">The typed refusal, when the Agent or this client refused.</param>
/// <param name="Failure">A transport or protocol failure; the outcome is then unknown.</param>
public sealed record DualBoundLiveViewCloseResult(
    bool Confirmed,
    bool RequestSent,
    DualBindingRefusal? Refusal,
    Exception? Failure)
{
    public long? CaptureAdmissionRemainingMs { get; init; }
}

/// <summary>
/// Typed client for the bound Live View (Issue #291). Holds one thing the Agent cannot tell the UI
/// on its own: whether the app may treat everything as closed.
/// </summary>
/// <remarks>
/// <para>
/// "Closed" is only ever concluded from a stop the Agent confirmed. A request that may have opened
/// something and whose answer is lost leaves <see cref="DualBoundLiveViewState.MaybeOpen"/>, and
/// nothing is started on top of it: <see cref="StartAsync"/> refuses locally until a stop confirms.
/// That is what lets the capture path ask "is everything closed?" and get an answer it can trust,
/// without sending anything when nothing was ever opened.
/// </para>
/// <para>
/// This class does not retry. A refusal or an exception is returned or thrown to the caller, who
/// decides; the only automatic effect is on its own state.
/// </para>
/// <para>
/// Session state (Ready, invalidated, activated) stays in <see cref="DualBindingSessionClient"/>;
/// this class asks it rather than keeping a copy.
/// </para>
/// </remarks>
public sealed class DualBoundLiveViewClient
{
    private readonly DualBindingSessionClient _session;
    private readonly Func<string> _requestIdFactory;
    private string _knownSessionId = string.Empty;

    public DualBoundLiveViewClient(DualBindingSessionClient session, Func<string>? requestIdFactory = null)
    {
        ArgumentNullException.ThrowIfNull(session);
        _session = session;
        _requestIdFactory = requestIdFactory ?? (() => $"boundlv-{Guid.NewGuid():N}");
    }

    private DualBoundLiveViewState _state = DualBoundLiveViewState.ConfirmedClosed;
    private string? _liveAlias;

    /// <summary>
    /// Reading it first adopts a new binding session if there is one, so a state left over from the
    /// previous session is never read as the new session's state.
    /// </summary>
    public DualBoundLiveViewState State
    {
        get
        {
            SynchronizeWithSession();
            return _state;
        }
        private set => _state = value;
    }

    /// <summary>The camera whose Live View is open, or null. Only set while <see cref="State"/> is Live.</summary>
    public string? LiveAlias
    {
        get
        {
            SynchronizeWithSession();
            return _liveAlias;
        }
        private set => _liveAlias = value;
    }

    /// <summary>
    /// The Agent may hold something open (or this client cannot say it does not), so a capture must
    /// first run <see cref="CloseAsync"/>.
    /// </summary>
    public bool MayBeOpen => State is DualBoundLiveViewState.Live or DualBoundLiveViewState.MaybeOpen;

    public async Task<DualBindingReply<DualBoundLiveViewStartResult>> StartAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default)
    {
        SynchronizeWithSession();
        if (_session.CheckBoundLiveViewAvailable() is { } unavailable)
        {
            return RefuseLocally<DualBoundLiveViewStartResult>(unavailable);
        }
        if (State != DualBoundLiveViewState.ConfirmedClosed)
        {
            // Never an implicit switch: a start on top of something that may be open is the
            // exact thing the Agent refuses too. The caller stops (and sees it confirmed) first.
            return DualBindingReply<DualBoundLiveViewStartResult>.Refused(new DualBindingRefusal
            {
                ResultCode = DualBoundLiveViewProtocol.RefusalCodes.AlreadyOpen,
                SessionId = _session.SessionId,
                State = _session.State,
                Detail = "A bound Live View may still be open; stop it before starting another.",
            });
        }

        var sessionId = _session.SessionId;
        var requestId = _requestIdFactory();
        var requestJson = DualBoundLiveViewProtocolCodec.CreateStartRequest(
            requestId, DualBoundLiveViewHost.Binding, sessionId, cameraAlias);

        // From here the Agent may have opened a source, whatever happens to the answer.
        State = DualBoundLiveViewState.MaybeOpen;
        LiveAlias = null;
        var responseJson = await _session.SendBoundLiveViewRequestAsync(requestJson, cancellationToken)
            .ConfigureAwait(false);
        var reply = DualBoundLiveViewProtocolCodec.DeserializeStartResponse(
            responseJson, requestId, DualBoundLiveViewHost.Binding, sessionId, cameraAlias);

        if (reply.Value is not null)
        {
            State = DualBoundLiveViewState.Live;
            LiveAlias = cameraAlias;
            return reply;
        }

        ApplyRefusal(reply.Refusal!, sdkMayHaveBeenCalled: !RefusedBeforeTouchingTheSdk(reply.Refusal!.ResultCode));
        return reply;
    }

    public async Task<DualBindingReply<DualBoundLiveViewFrameResult>> GetFrameAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default)
    {
        SynchronizeWithSession();
        if (_session.CheckBoundLiveViewAvailable() is { } unavailable)
        {
            return RefuseLocally<DualBoundLiveViewFrameResult>(unavailable);
        }
        if (State != DualBoundLiveViewState.Live || !string.Equals(LiveAlias, cameraAlias, StringComparison.Ordinal))
        {
            return DualBindingReply<DualBoundLiveViewFrameResult>.Refused(new DualBindingRefusal
            {
                ResultCode = DualBoundLiveViewProtocol.RefusalCodes.NotActive,
                SessionId = _session.SessionId,
                State = _session.State,
                Detail = "No bound Live View is open for this camera.",
            });
        }

        var sessionId = _session.SessionId;
        var requestId = _requestIdFactory();
        var requestJson = DualBoundLiveViewProtocolCodec.CreateFrameRequest(
            requestId, DualBoundLiveViewHost.Binding, sessionId, cameraAlias);
        var responseJson = await _session.SendBoundLiveViewRequestAsync(requestJson, cancellationToken)
            .ConfigureAwait(false);
        var reply = DualBoundLiveViewProtocolCodec.DeserializeFrameResponse(
            responseJson, requestId, DualBoundLiveViewHost.Binding, sessionId, cameraAlias);

        if (reply.Value is null)
        {
            // An empty or oversized frame leaves the Live View open on the Agent side, and keeps
            // it Live here. Anything the Agent says about the Live View itself changes the state.
            var refusal = reply.Refusal!;
            if (refusal.ResultCode is DualBoundLiveViewProtocol.RefusalCodes.FrameUnavailable or
                DualBoundLiveViewProtocol.RefusalCodes.FrameTooLarge)
            {
                _session.ApplyBoundLiveViewRefusal(refusal);
            }
            else
            {
                ApplyRefusal(refusal, sdkMayHaveBeenCalled: true);
            }
        }

        return reply;
    }

    /// <summary>
    /// Sends <c>stop-bound-live-view</c>. Idempotent on the Agent. The state returns to
    /// ConfirmedClosed only when both the Live View and the SDK source are reported closed.
    /// </summary>
    public async Task<DualBindingReply<DualBoundLiveViewStopResult>> StopAsync(
        CancellationToken cancellationToken = default)
    {
        SynchronizeWithSession();
        if (_session.CheckBoundLiveViewAvailable() is { } unavailable)
        {
            return RefuseLocally<DualBoundLiveViewStopResult>(unavailable);
        }

        var sessionId = _session.SessionId;
        var requestId = _requestIdFactory();
        var requestJson = DualBoundLiveViewProtocolCodec.CreateStopRequest(
            requestId, DualBoundLiveViewHost.Binding, sessionId);

        if (State == DualBoundLiveViewState.Live)
        {
            // From here the Live View is no longer something this client can say is running.
            State = DualBoundLiveViewState.MaybeOpen;
            LiveAlias = null;
        }

        var responseJson = await _session.SendBoundLiveViewRequestAsync(requestJson, cancellationToken)
            .ConfigureAwait(false);
        var reply = DualBoundLiveViewProtocolCodec.DeserializeStopResponse(
            responseJson, requestId, DualBoundLiveViewHost.Binding, sessionId);

        if (reply.Value is { } stopped)
        {
            if (stopped.ConfirmedClosed)
            {
                State = DualBoundLiveViewState.ConfirmedClosed;
                LiveAlias = null;
            }
            else
            {
                State = DualBoundLiveViewState.MaybeOpen;
            }
        }
        else
        {
            ApplyRefusal(reply.Refusal!, sdkMayHaveBeenCalled: true);
        }

        return reply;
    }

    /// <summary>
    /// Makes sure everything is confirmed closed, sending nothing when nothing was ever opened (so
    /// a capture that never used the Live View sends the Agent exactly what it did before). A
    /// transport or protocol failure is returned in the result rather than thrown: the caller's
    /// next step is the same either way (do not proceed), and the outcome is "unknown", not "closed".
    /// </summary>
    public async Task<DualBoundLiveViewCloseResult> CloseAsync(CancellationToken cancellationToken = default)
    {
        SynchronizeWithSession();
        if (State == DualBoundLiveViewState.ConfirmedClosed)
        {
            return new DualBoundLiveViewCloseResult(Confirmed: true, RequestSent: false, Refusal: null, Failure: null);
        }

        try
        {
            var reply = await StopAsync(cancellationToken).ConfigureAwait(false);
            var remaining = reply.Value?.CaptureAdmissionRemainingMs;
            return new DualBoundLiveViewCloseResult(
                Confirmed: State == DualBoundLiveViewState.ConfirmedClosed,
                RequestSent: reply.Refusal is null || !IsLocalRefusal(reply.Refusal),
                Refusal: reply.Refusal,
                Failure: null)
            {
                CaptureAdmissionRemainingMs = remaining,
            };
        }
        catch (Exception exception) when (exception is not (OperationCanceledException or OutOfMemoryException))
        {
            // The request may have reached the Agent; the answer did not come back as a stop
            // result. Whatever was open stays "maybe open".
            if (State == DualBoundLiveViewState.Live)
            {
                State = DualBoundLiveViewState.MaybeOpen;
            }

            return new DualBoundLiveViewCloseResult(Confirmed: false, RequestSent: true, Refusal: null, Failure: exception);
        }
    }

    /// <summary>
    /// The caller decided the binding cannot be used (a stop it needed could not be confirmed, or
    /// the transport failed mid-operation). Applies <paramref name="refusal"/> to the session the
    /// same way an Agent refusal would be, and stops this client from opening or stopping anything
    /// until a fresh binding.
    /// </summary>
    public void MarkBindingUnusable(DualBindingRefusal refusal)
    {
        ArgumentNullException.ThrowIfNull(refusal);
        _session.ApplyBoundLiveViewRefusal(refusal);
        State = DualBoundLiveViewState.Invalidated;
        LiveAlias = null;
    }

    // Agent-side refusals that leave nothing opened: the ones decided before any SDK call (form,
    // readiness, host lifetime), and LiveViewStartFailed, which the Agent only answers when the SDK
    // refused the start with nothing left open. The request opened nothing, so the state it started
    // from is the state it is in. Every other refusal is treated as "something may be open", and the
    // caller has to stop and confirm.
    private static bool RefusedBeforeTouchingTheSdk(string resultCode) =>
        resultCode is DualBoundLiveViewProtocol.RefusalCodes.BindingNotReady or
            DualBoundLiveViewProtocol.RefusalCodes.CaptureTransactionActive or
            DualBoundLiveViewProtocol.RefusalCodes.HostLifetimeInsufficient or
            DualBoundLiveViewProtocol.RefusalCodes.ProtocolRequired or
            DualBoundLiveViewProtocol.RefusalCodes.Unavailable or
            DualBoundLiveViewProtocol.RefusalCodes.StartFailed or
            "UnexpectedField" or "InvalidFieldType" or "InvalidRequestId" or "InvalidSessionId" or
            "UnknownCameraAlias" or "UnsupportedOperation";

    private static bool IsLocalRefusal(DualBindingRefusal refusal) =>
        refusal.ResultCode is "NoBindingSession" or "CaptureAlreadyActivated" or "BindingHostExpired" or
            DualBoundLiveViewProtocol.RefusalCodes.BindingNotReady;

    /// <summary>The binding is gone (or going): the Agent invalidated it, or its state says Invalid.</summary>
    internal static bool NeedsRebinding(DualBindingRefusal refusal) =>
        refusal.RequiresRebinding || refusal.State == DualBindingSessionState.Invalid;

    private void ApplyRefusal(DualBindingRefusal refusal, bool sdkMayHaveBeenCalled)
    {
        _session.ApplyBoundLiveViewRefusal(refusal);
        if (NeedsRebinding(refusal))
        {
            State = DualBoundLiveViewState.Invalidated;
            LiveAlias = null;
            return;
        }

        if (!sdkMayHaveBeenCalled)
        {
            // Nothing was opened by this request. Where the request started from a closed state
            // (the only state a start is sent from), it is closed again.
            State = DualBoundLiveViewState.ConfirmedClosed;
            LiveAlias = null;
            return;
        }

        // The Agent refused after (or while) touching the SDK, or says something is open that this
        // client did not believe was: whatever is open has to be stopped and confirmed.
        State = DualBoundLiveViewState.MaybeOpen;
        LiveAlias = null;
    }

    private DualBindingReply<T> RefuseLocally<T>(DualBindingRefusal refusal) where T : class
    {
        // A session that is gone (host exited, binding invalidated, or never started) leaves
        // nothing this client can stop or open.
        if (NeedsRebinding(refusal) || refusal.ResultCode == "NoBindingSession")
        {
            State = DualBoundLiveViewState.Invalidated;
            LiveAlias = null;
        }

        return DualBindingReply<T>.Refused(refusal);
    }

    private void SynchronizeWithSession()
    {
        // A new binding session starts with nothing open, whatever the previous one left behind.
        // An empty id (the session was cleared) is not a new session: the state stays as it was,
        // and a start on it is refused until a session exists again.
        if (_session.SessionId.Length > 0 &&
            !string.Equals(_knownSessionId, _session.SessionId, StringComparison.Ordinal))
        {
            _knownSessionId = _session.SessionId;
            State = DualBoundLiveViewState.ConfirmedClosed;
            LiveAlias = null;
        }
    }
}
