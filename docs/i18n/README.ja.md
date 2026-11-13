**言語：** [English](../../README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português (Brasil)](README.pt-BR.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [一覧](README.md)

**Discord:** [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM)

# Guarana

**統合GPU向けに調整された Nintendo Switch 2 エミュレーター。**

Guarana は [Lichii](https://github.com/zortos293/Lichii)（`nx2` ブランチ）から派生したオープンソースプロジェクトで、Windows x64 向けです。内蔵グラフィックスのノートPCや携帯型機器を対象とし、まず Intel Arc、次に AMD Radeon に対応します。フレームペースの安定、フレーム生成、高速な起動、電力配分の最適化に取り組んでいます。

> [!IMPORTANT]
> Guarana には鍵、ファームウェア、ゲーム、システムファイルは**含まれていません**。ご自身が所有する本体とゲームから取得したものを用意してください。Nintendo および Lichii プロジェクトとは提携していません。下記の法的注意事項をご覧ください。

## 概要

- GPL-3.0-or-later で配布される、無料のオープンソースエミュレーターです。
- 共有メモリと電力に制約のある統合GPU向けに調整されています。
- Windows x64、AVX2 対応の CPU、最新のグラフィックスドライバーが必要です。
- 鍵、ファームウェア、ゲーム、システムファイルは含まれていません。

## 計測結果

ノートPC（Core Ultra 9 285H、Arc 140T 内蔵GPU、Intel ドライバー 32.0.101.9175、メモリ 32 GB）で計測しました。*Ocarina of Time* の Hyrule Field で、グローバルメモリのルーティング修正（`EDEN_NX2_GM=11`）を有効にしたところ、ゲーム自身が描画するフレームレートは **14.8 fps** から **22.8 fps** に向上しました。同じ場所でスイッチをオン・オフして比較し、フレーム生成は無効にしています。

これは1台の機器、1つのゲーム、1つのシーンによる結果です。他の機器、ドライバー、ゲーム、シーンは未計測です。[計測へのご協力](../../docs/BENCHMARKING.md)をお待ちしています。

## 対応状況

現在テストしているのは Arc 140T のみです。その他の GPU（Arc 140V、Arc B390、Radeon 780M、880M、890M など）はまだテストしていません。[デバイスレポート](../../docs/DEVICES.md#send-a-device-report)をお寄せください。共有メモリが 16 GB の機器は推奨下限を下回るため、詳細は [DEVICES.md](../../docs/DEVICES.md) を参照してください。

## クイックスタート

1. [Releases](https://github.com/karimalsalah/guarana/releases) から最新の zip をダウンロードします。公式の配布元はこのページのみです。
2. zip の中身をすべて、書き込み可能なフォルダーに展開し、**`lichii.exe`** を実行します。
3. 鍵とファームウェアをインストールします。**Tools → Install Decryption Keys**、次に **Tools → Install Firmware** の順に選択します。
4. ゲームフォルダーを追加し（空のゲーム一覧をダブルクリック）、ゲームを起動します。各エリアに初めて入ったときはシェーダーのコンパイルでカクつくことがあります。以降はキャッシュから読み込まれます。
5. 任意：**Emulation → Configure… → Graphics** で **Enable Frame Generation** を有効にします。

設定、セーブデータ、キャッシュは `%APPDATA%\lichii` に保存されます。これは上流の Lichii と同じフォルダーです。切り替える前にバックアップを取ってください。すべてのスイッチは [docs/switches.md](../../docs/switches.md)、よくある問題は [TROUBLESHOOTING.md](../../docs/TROUBLESHOOTING.md) にあります。

## 鍵、ファームウェア、ゲームについて（法的注意）

- 鍵、ファームウェア、ゲームは、ご自身が所有する機器から取得したものを使用してください。Guarana はこれらを提供せず、issue、PR、ディスカッションでの共有も受け付けていません。
- 著作権で保護されたファイルの要求やリンク、入手方法に関する質問には対応しません。
- Guarana は独立したプロジェクトです。Nintendo や Lichii プロジェクトの提携、承認、支援は受けていません。「Nintendo」と「Nintendo Switch」は Nintendo の商標であり、本ソフトウェアの互換対象を示すためだけに使用しています。全文は [LEGAL.md](../../LEGAL.md) をご覧ください。

## 貢献について

- **コードを書かずに貢献：** [ベンチマーク](../../docs/BENCHMARKING.md)を実行して結果を送るか、[デバイスレポート](../../docs/DEVICES.md#send-a-device-report)を提出してください。
- **コードで貢献：** `good first issue` ラベルの付いた issue から始め、[CONTRIBUTING.md](../../CONTRIBUTING.md) と[ビルドガイド](../../docs/Build.md)を読んでください。
- **AI エージェント：** まず [AGENTS.md](../../AGENTS.md)、[アーキテクチャ](../../docs/ARCHITECTURE.md)、[AI ポリシー](../../docs/AI_POLICY.md)を確認してください。すべての PR には人間の責任者が必要です。

## リンク

- [英語版 README（正本）](../../README.md) · [公式サイト](https://karimalsalah.github.io/guarana/) · [FAQ](../../docs/FAQ.md)
- [ロードマップ](../../ROADMAP.md) · [変更履歴](../../CHANGELOG.md) · [ライセンス](../../LICENSE.txt)

## ライセンス

Guarana は **GNU General Public License v3.0 以降** のもとで提供される自由ソフトウェアです。

この翻訳は英語版より遅れている場合があります。差異がある場合は英語版 README が優先されます。翻訳はまだネイティブスピーカーによる確認を受けていません。
