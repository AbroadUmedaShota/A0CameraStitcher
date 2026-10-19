# A/Bライブビューの水準器（#342）

通常の `scripts/Start-WpdDualLivePreview.ps1` 起動で、各画面のAF操作欄の下に、左右・前後の受信角度と左右の傾き目盛りを表示する。既存のA/B登録とカメラ制御leaseを使う。映像と角度は同じ復号済みフレーム・同じ時刻で更新する。追加のカメラ命令や設定書き込みはない。

数値は整数度の参考値。精度・センサー有効性は未検証で、水平合格や図面との平行を判定しない。角度が利用不可のフレームでは以前の値を残さない。起動前・停止中・失敗時・2秒以上更新が途切れた場合は数値と傾き目盛りを隠し、状態を表示する。受信した0度と未取得を区別する。

ニコンの[公式FAQ](https://nij.nikon.com/support/faq/products/article?articleNo=000066691)は、D810を含む対象機で大きな前後傾斜による誤差や測定できない姿勢を説明している。真下を向ける最終吊り下げ配置の平行度や合成精度は、この参考値だけでは確認できない。制限を各画面に常時表示する。最終リグでの校正・図面実測は別工程のままとする。

## データの根拠と表示

既存NOTICEと同じ公開digiCamControl固定commitの [NikonD600Base](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/Nikon/NikonD600Base.cs)（D810の継承先）で、384-byteヘッダーのoffset 52/56をroll/pitchとして読む。[BaseMTPCamera.ToInt16](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl.Devices/BaseMTPCamera.cs)はsigned big-endian 16-bitで、[LiveViewWnd.xaml](https://github.com/dukus/digiCamControl/blob/a95ee52bbc1e192a7d1da038f3af6561b185d312/CameraControl/windows/LiveViewWnd.xaml)ではrollを度のRotateTransformへ渡す。整数wordのみを採用し、隣の2byteの小数形式や有効性flagを推測しない。upstreamのHaveLevelAngleData=trueは固定代入であり、センサー有効性の証拠とは扱わない。SDK同梱資料は利用していない。

実D810から358/359度が届いたため、一周以内（−360〜360度）の値を等価な−180〜180度へ折り返す。例: 358→−2、359→−1、±360→0。範囲外は利用不可にし、映像自体は継続する。この数値範囲はデータの妥当性検査で、水平・光学品質の閾値ではない。琥珀色の線は参考表示で、0度でも合格色へ切り替えない。yaw/方位、ゼロ点校正、合成への自動適用は含まない。

## 検証方法

- `d810_preview_contracts`: byte順・負値・360度折り返し・境界・異常値・JPEG envelope・鮮度/停止/未取得。
- `preview_level_ui_contracts`: `--level-ui-smoke` でカメラを列挙せず、自分のメモリbitmapだけで0/90度の描画pixel、停止/更新切れ/未取得時の目盛り非表示、繰り返し描画のGDI resource、既定サイズの実font文字幅/折返し高さを検査。
- 明示的な実機確認: `A0CameraStitcher.WpdDualPreview.exe --seconds 20 --level-smoke`。通常Liveに加え、各側20フレーム以上の角度取得と10フレーム以上のPresent成功を要求する。自動撮影・AF・レンズ駆動は行わない。

実描画の計数は画面へbackbufferをPresentできた後だけ増やす。取得/描画の成功と、カメラを傾けたときの追従・本体水準器との照合・精度受入は別の確認とする。最新の実機結果は[開発状況](DEVELOPMENT_STATUS_20261016.md)に記録する。
