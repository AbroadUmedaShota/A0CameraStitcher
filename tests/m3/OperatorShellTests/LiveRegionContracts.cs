using System.ComponentModel;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using A0CameraStitcher.M3.OperatorShell.Controls;

// Issue #238: LiveSetting を付けた TextBlock の文字が変わった時に LiveRegionChanged を発火する仕組みの試験。
// UI Automation のクライアントが居ない環境でも観測できるよう、LiveRegion の差し替え口で
// 「ピアへ LiveRegionChanged を発火する呼び出し」を記録し、回数と対象のピアで確かめる。
internal static class LiveRegionContracts
{
    internal static void Run()
    {
        var originalListenerExists = LiveRegion.ListenerExists;
        var originalRaise = LiveRegion.RaiseLiveRegionChanged;
        var raised = new List<AutomationPeer>();
        LiveRegion.ListenerExists = static () => true;
        LiveRegion.RaiseLiveRegionChanged = peer => raised.Add(peer);
        Window? window = null;
        try
        {
            var polite = new TextBlock { Text = "Ready" };
            AutomationProperties.SetLiveSetting(polite, AutomationLiveSetting.Polite);
            LiveRegion.SetAnnounce(polite, true);

            var assertive = new TextBlock();
            AutomationProperties.SetLiveSetting(assertive, AutomationLiveSetting.Assertive);
            LiveRegion.SetAnnounce(assertive, true);

            var off = new TextBlock { Text = "Off" };
            LiveRegion.SetAnnounce(off, true);

            var withoutAnnounce = new TextBlock { Text = "NoAnnounce" };
            AutomationProperties.SetLiveSetting(withoutAnnounce, AutomationLiveSetting.Polite);

            var bound = new TextBlock();
            AutomationProperties.SetLiveSetting(bound, AutomationLiveSetting.Polite);
            LiveRegion.SetAnnounce(bound, true);
            bound.SetBinding(TextBlock.TextProperty, new System.Windows.Data.Binding(nameof(TextSource.Message)));

            var hidden = new TextBlock { Visibility = Visibility.Collapsed };
            AutomationProperties.SetLiveSetting(hidden, AutomationLiveSetting.Polite);
            LiveRegion.SetAnnounce(hidden, true);

            var panel = new StackPanel();
            foreach (var element in new[] { polite, assertive, off, withoutAnnounce, bound, hidden })
            {
                panel.Children.Add(element);
            }

            window = new Window
            {
                Content = panel,
                Width = 200,
                Height = 200,
                Left = -10000,
                Top = -10000,
                ShowActivated = false,
                ShowInTaskbar = false,
                WindowStyle = WindowStyle.None,
            };
            var source = new TextSource();
            window.DataContext = source;

            // 読み込み前の変化は通知しない（DataContext の初期反映で全領域が一斉に読み上げられるのを防ぐ）。
            polite.Text = "Before show";
            source.Message = "Bound before show";
            Check.Equal(0, raised.Count);

            window.Show();
            // 表示直後に束縛が最初の値を反映する分は通知しない。Loaded が済むまで待つ。
            window.Dispatcher.Invoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle, static () => { });
            Check.True(window.IsLoaded, "The test window must be loaded before text changes are observed.");
            Check.True(hidden.IsLoaded, "A collapsed TextBlock must still be Loaded once its window is loaded; the pending-until-visible path relies on it.");
            Check.Equal(0, raised.Count);

            // 文字が変わると、そのテキストのピアから 1 回発火する。ピアは未作成でも作って使う。
            polite.Text = "Saved";
            Check.Equal(1, raised.Count);
            Check.True(
                raised[0] is UIElementAutomationPeer { Owner: var owner } && ReferenceEquals(owner, polite),
                "The peer raised for a changed TextBlock must be that TextBlock's own peer.");
            Check.True(
                raised[0].GetLiveSetting() == AutomationLiveSetting.Polite,
                "The peer must report the LiveSetting declared in markup.");

            // 同じ文字の再設定は WPF が変更通知を出さないので発火しない。
            polite.Text = "Saved";
            Check.Equal(1, raised.Count);

            // 空・空白への変化では発火しない。空を挟めば、同じ文言でもう一度発火する（#228 の仕組み）。
            polite.Text = string.Empty;
            Check.Equal(1, raised.Count);
            polite.Text = "   ";
            Check.Equal(1, raised.Count);
            polite.Text = "Saved";
            Check.Equal(2, raised.Count);

            // 別の文字への変化は毎回発火する。
            polite.Text = "Saved again";
            Check.Equal(3, raised.Count);

            // 初期値が空の領域も、後から文字が入れば発火し、対象は自分のピア。
            assertive.Text = "Blocked";
            Check.Equal(4, raised.Count);
            Check.True(
                raised[3] is UIElementAutomationPeer { Owner: var assertiveOwner } && ReferenceEquals(assertiveOwner, assertive)
                && raised[3].GetLiveSetting() == AutomationLiveSetting.Assertive,
                "An assertive region must raise from its own peer and keep its Assertive setting.");

            // LiveSetting が Off／Announce 無しは発火しない。
            off.Text = "Off changed";
            withoutAnnounce.Text = "NoAnnounce changed";
            Check.Equal(4, raised.Count);

            // 束縛した Text の変化でも発火する。
            source.Message = "Bound changed";
            Check.Equal(5, raised.Count);
            Check.True(
                raised[4] is UIElementAutomationPeer { Owner: var boundOwner } && ReferenceEquals(boundOwner, bound),
                "A bound Text change must raise from the bound TextBlock's peer.");
            source.Message = string.Empty;
            Check.Equal(5, raised.Count);

            // 非表示の間に変わった文字は、表示された時に 1 回だけ発火する。非表示の間は発火しない。
            hidden.Text = "Notice";
            Check.Equal(5, raised.Count);
            hidden.Visibility = Visibility.Visible;
            Check.Equal(6, raised.Count);
            hidden.Visibility = Visibility.Collapsed;
            hidden.Visibility = Visibility.Visible;
            Check.Equal(6, raised.Count);

            // 非表示の間に空へ戻ったら、表示しても発火しない。
            hidden.Visibility = Visibility.Collapsed;
            hidden.Text = "Transient";
            hidden.Text = string.Empty;
            hidden.Visibility = Visibility.Visible;
            Check.Equal(6, raised.Count);

            // 購読者（UI Automation のクライアント）が居なければ発火しない。
            LiveRegion.ListenerExists = static () => false;
            polite.Text = "No listener";
            Check.Equal(6, raised.Count);
            LiveRegion.ListenerExists = static () => true;

            // Announce を外せば監視を止める。
            LiveRegion.SetAnnounce(polite, false);
            polite.Text = "Detached";
            Check.Equal(6, raised.Count);

            // 既定の差し替え口（実際の購読者の有無を見て、ピアから発火）でも例外なく動く。
            LiveRegion.ListenerExists = originalListenerExists;
            LiveRegion.RaiseLiveRegionChanged = originalRaise;
            assertive.Text = "Default path";
            Check.Equal(6, raised.Count);
        }
        finally
        {
            LiveRegion.ListenerExists = originalListenerExists;
            LiveRegion.RaiseLiveRegionChanged = originalRaise;
            window?.Close();
        }
    }

    private sealed class TextSource : INotifyPropertyChanged
    {
        private string _message = string.Empty;

        public event PropertyChangedEventHandler? PropertyChanged;

        public string Message
        {
            get => _message;
            set
            {
                if (_message == value)
                {
                    return;
                }

                _message = value;
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Message)));
            }
        }
    }
}
