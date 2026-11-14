// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <optional>
#include <vector>
#include <QWidget>

class QTimer;

#include "common/dkb_cpu_trace.h"
#include "core/perf_stats.h"

namespace VideoCore {
class ShaderNotify;
}

class MainWindow;

class PerformanceOverlay : public QWidget {
    Q_OBJECT

public:
    explicit PerformanceOverlay(MainWindow* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void resetPosition(const QPoint& pos);
    void updateStats(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);
    void paintGraph(QPainter& painter, const QRectF& area);
    void paintCompact(QPainter& painter);
    void paintMeters(QPainter& painter, const QRectF& area);
    void paintSheen(QPainter& painter);
    void applyLayout();
    void advanceMotion();

    bool m_compact = false;
    double m_generatedFps = 0.0;
    bool m_frameGenOn = false;
    /// 0–1 loop that drives the sheen and the trace head. Stats still arrive on their own clock.
    qreal m_phase = 0;
    bool m_smooth = false;
    double m_smoothFps = 0;
    double m_smoothMs = 0;
    double m_smoothCpu = 0;
    double m_smoothGpu = 0;
    double m_speed = 0;
    int m_shadersBuilding = 0;
    bool m_hitch = false;
    QTimer* m_motion = nullptr;

    MainWindow* m_mainWindow = nullptr;

    QPoint m_offset{25, 75};

    // drag
    QPoint m_drag_start_pos;

    std::optional<double> m_fps;
    /// The last 5 s of 500 ms fps readings, for min/median/max
    std::deque<float> m_fpsSamples;
    std::optional<double> m_frameMs;
    std::optional<double> m_cpuPercent;
    std::optional<double> m_gpuPercent;
    std::optional<Common::DkbCpuTrace::Snapshot> m_cpuSnapshot;
    std::vector<Core::FrameTimeHistory::Entry> m_frames;
    QString m_tags;

signals:
    void closed();
};
