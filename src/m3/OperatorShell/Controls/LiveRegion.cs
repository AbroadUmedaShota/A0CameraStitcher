using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Data;

namespace A0CameraStitcher.M3.OperatorShell.Controls;

/// <summary>
/// 文字の変化をスクリーンリーダーへ通知する添付プロパティ（Issue #238）。
/// <c>AutomationProperties.LiveSetting</c> は「この要素は通知領域である」という宣言にすぎず、
/// 文字が変わっても WPF は <see cref="AutomationEvents.LiveRegionChanged"/> を自動では発火しない。
/// ここでは TextBlock の Text を監視し、変化のたびにピアから同イベントを発火する。
/// <para>
/// 発火しない場合: ①空（空白のみを含む）への変化 ②LiveSetting が Off／未設定 ③要素の読み込み（Loaded）が済む前
/// （DataContext の初期反映で全領域が一斉に読み上げられるのを防ぐ）。
/// 非表示の間に変わった文字は、表示された時点で 1 回だけ通知する。
/// 同じ文字の再設定は WPF が変更通知を出さないため通知されない。同じ文言をもう一度読み上げたい場合は、
/// 空を挟んで再設定する（#228 の仕組みを維持する）。
/// </para>
/// </summary>
public static class LiveRegion
{
    public static readonly DependencyProperty AnnounceProperty = DependencyProperty.RegisterAttached(
        "Announce",
        typeof(bool),
        typeof(LiveRegion),
        new PropertyMetadata(false, OnAnnounceChanged));

    // TextBlock.Text を自分自身へ OneWay で束縛して変更を受け取る。DependencyPropertyDescriptor の
    // AddValueChanged と違い、静的な参照を作らないので要素が解放されなくなることがない。
    private static readonly DependencyProperty ObservedTextProperty = DependencyProperty.RegisterAttached(
        "ObservedText",
        typeof(string),
        typeof(LiveRegion),
        new PropertyMetadata(null, OnObservedTextChanged));

    // 非表示の間に文字が変わった（表示時に通知が必要）ことを覚える。
    private static readonly DependencyProperty PendingAnnouncementProperty = DependencyProperty.RegisterAttached(
        "PendingAnnouncement",
        typeof(bool),
        typeof(LiveRegion),
        new PropertyMetadata(false));

    /// <summary>
    /// 試験用の差し替え口。UI Automation のクライアント（ナレーター等）が居ない環境では
    /// <see cref="AutomationPeer.ListenerExists"/> が false になるため、発火の有無を観測できない。
    /// 既定は実際の購読者の有無を見る。
    /// </summary>
    internal static Func<bool> ListenerExists { get; set; } =
        static () => AutomationPeer.ListenerExists(AutomationEvents.LiveRegionChanged);

    /// <summary>試験用の差し替え口。既定はピアから LiveRegionChanged を発火する。</summary>
    internal static Action<AutomationPeer> RaiseLiveRegionChanged { get; set; } =
        static peer => peer.RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);

    public static bool GetAnnounce(TextBlock element)
    {
        ArgumentNullException.ThrowIfNull(element);
        return (bool)element.GetValue(AnnounceProperty);
    }

    public static void SetAnnounce(TextBlock element, bool value)
    {
        ArgumentNullException.ThrowIfNull(element);
        element.SetValue(AnnounceProperty, value);
    }

    private static void OnAnnounceChanged(DependencyObject sender, DependencyPropertyChangedEventArgs e)
    {
        if (sender is not TextBlock textBlock)
        {
            return;
        }

        if ((bool)e.NewValue)
        {
            textBlock.IsVisibleChanged += OnIsVisibleChanged;
            BindingOperations.SetBinding(
                textBlock,
                ObservedTextProperty,
                new Binding
                {
                    Source = textBlock,
                    Path = new PropertyPath(TextBlock.TextProperty),
                    Mode = BindingMode.OneWay,
                });
        }
        else
        {
            textBlock.IsVisibleChanged -= OnIsVisibleChanged;
            BindingOperations.ClearBinding(textBlock, ObservedTextProperty);
            textBlock.ClearValue(PendingAnnouncementProperty);
        }
    }

    private static void OnObservedTextChanged(DependencyObject sender, DependencyPropertyChangedEventArgs e)
    {
        // 束縛が最初に値を反映する時（旧値が null）は現在の表示を記録するだけで、変化ではない。
        if (sender is not TextBlock textBlock || e.OldValue is null)
        {
            return;
        }

        if (string.IsNullOrWhiteSpace(e.NewValue as string))
        {
            textBlock.ClearValue(PendingAnnouncementProperty);
            return;
        }

        if (!textBlock.IsLoaded)
        {
            // 読み込み前。表示の準備中に束縛が最初の値を反映する分は通知せず、
            // 読み込みが済んだ後に変わった文字だけを通知する。
            return;
        }

        if (!textBlock.IsVisible)
        {
            textBlock.SetValue(PendingAnnouncementProperty, true);
            return;
        }

        Raise(textBlock);
    }

    private static void OnIsVisibleChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (sender is not TextBlock textBlock || !(bool)e.NewValue)
        {
            return;
        }

        if (!(bool)textBlock.GetValue(PendingAnnouncementProperty))
        {
            return;
        }

        textBlock.ClearValue(PendingAnnouncementProperty);
        if (!string.IsNullOrWhiteSpace(textBlock.Text))
        {
            Raise(textBlock);
        }
    }

    private static void Raise(TextBlock textBlock)
    {
        if (AutomationProperties.GetLiveSetting(textBlock) == AutomationLiveSetting.Off)
        {
            return;
        }

        if (!ListenerExists())
        {
            return;
        }

        var peer = UIElementAutomationPeer.FromElement(textBlock)
            ?? UIElementAutomationPeer.CreatePeerForElement(textBlock);
        if (peer is null)
        {
            return;
        }

        RaiseLiveRegionChanged(peer);
    }
}
