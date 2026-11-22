// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <vector>

#include <QWidget>

class QPainter;

#include "common/dkb_cpu_trace.h"
#include "core/perf_stats.h"
#include "lichii/render/performance_overlay_model.h"

class QTimer;

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
    void contextMenuEvent(QContextMenuEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

signals:
    void closed();

private:
    struct Layout {
        int width = 0;
        int height = 0;
        qreal pad = 0;
        qreal gap = 0;
        qreal gutter = 0;
        bool detailed = false;
        double window_s = 2;
        QRectF header;
        QRectF meta;
        QRectF raster;
        QRectF spread;
        QRectF threads;
        QRectF memory;
        QRectF footer;
    };

    [[nodiscard]] Layout makeLayout() const;
    void resetPosition(const QPoint& pos);
    void updateStats(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);
    void applyLayout();
    void advanceMotion();
    void sampleLive(double window_s, bool force);
    void sampleMemory();
    void disarmSprings();
    [[nodiscard]] QString limiterText() const;
    [[nodiscard]] QString statusText() const;

    void paintChrome(QPainter& painter) const;
    void paintHeader(QPainter& painter, const Layout& layout) const;
    void paintMeta(QPainter& painter, const Layout& layout) const;
    void paintRaster(QPainter& painter, const Layout& layout) const;
    void paintSpread(QPainter& painter, const Layout& layout) const;
    void paintThreads(QPainter& painter, const Layout& layout) const;
    void paintMemory(QPainter& painter, const Layout& layout) const;
    void paintFooter(QPainter& painter, const Layout& layout) const;

    bool m_compact = false;
    double m_generatedFps = 0;
    bool m_frameGenOn = false;
    double m_speed = 0;
    int m_shadersBuilding = 0;
    QString m_tags;
    qreal m_phase = 0;
    double m_flash = 0;
    bool m_hitch = false;
    bool m_hitchLatch = false;
    double m_frameWait = 1;
    double m_cpuWait = 1;
    bool m_tickStarted = false;
    std::chrono::steady_clock::time_point m_tick{};

    Guarana::Overlay::Spring m_fpsSpring;
    Guarana::Overlay::Spring m_shownSpring;
    Guarana::Overlay::Spring m_msSpring;
    Guarana::Overlay::Spring m_lowSpring;
    Guarana::Overlay::Spring m_cpuSpring;
    Guarana::Overlay::Spring m_gpuSpring;
    std::array<Guarana::Overlay::Spring, Guarana::Overlay::HistogramBins> m_binSpring{};
    std::array<Guarana::Overlay::Spring, Common::DkbCpuTrace::RoleCount> m_roleSpring{};
    std::array<double, Common::DkbCpuTrace::RoleCount> m_roleTarget{};
    std::array<bool, Common::DkbCpuTrace::RoleCount> m_roleKnown{};
    std::array<int, Guarana::Overlay::HistogramBins> m_bins{};

    std::optional<double> m_fps;
    std::optional<double> m_frameMs;
    std::optional<double> m_cpuPercent;
    std::optional<double> m_gpuPercent;
    std::optional<double> m_lowFps;
    std::optional<double> m_lowMs;
    std::optional<double> m_gpuBusyMs;
    std::optional<Guarana::Overlay::MemoryFigure> m_ram;
    std::optional<Guarana::Overlay::MemoryFigure> m_vram;
    bool m_vramShared = false;
    std::optional<std::chrono::steady_clock::time_point> m_memorySampled;
    std::optional<Common::DkbCpuTrace::Snapshot> m_cpuSnapshot;
    std::deque<float> m_fpsSamples;
    std::vector<Core::FrameTimeHistory::Entry> m_frames;
    std::vector<float> m_msScratch;
    Guarana::Overlay::Limiter m_limiter = Guarana::Overlay::Limiter::Unknown;

    QTimer* m_motion = nullptr;
    MainWindow* m_mainWindow = nullptr;
    QPoint m_offset{25, 75};
    QPoint m_drag_start_pos;
};
