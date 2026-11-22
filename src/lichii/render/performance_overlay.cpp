// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "performance_overlay.h"

#include <algorithm>
#include <cmath>

#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStringList>
#include <QTimer>
#include <QWindow>

#include "common/memory_detect.h"
#include "common/settings.h"
#include "core/core.h"
#include "main_window.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace {

using Guarana::Overlay::Budget120Ms;
using Guarana::Overlay::Budget30Ms;
using Guarana::Overlay::Budget45Ms;
using Guarana::Overlay::Budget60Ms;
using Guarana::Overlay::HistogramBins;

constexpr std::size_t RoleCount = Common::DkbCpuTrace::RoleCount;
constexpr int FpsSamples = 10;

constexpr std::array<const char*, RoleCount> LaneLabels{
    "C0", "C1", "C2", "C3", "C4", "C5", "SC", "GC", "GB", "VK",
};
constexpr std::array<const char*, HistogramBins> BinLabels{"120", "90", "60", "45",
                                                           "30",  "20", "10", "+"};
constexpr std::array<double, HistogramBins> BinToneMs{8.3,  11.1, 16.7,  22.2,
                                                      33.3, 50.0, 100.0, 140.0};

QString Dash() {
    return QStringLiteral("—");
}

QFont MonoFont(int pixel_size, QFont::Weight weight = QFont::Normal) {
    QFont font;
    font.setFamilies({QStringLiteral("Geist Mono"), QStringLiteral("Cascadia Mono"),
                      QStringLiteral("JetBrains Mono"), QStringLiteral("Consolas"),
                      QStringLiteral("SF Mono"), QStringLiteral("Menlo"),
                      QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono")});
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(pixel_size);
    font.setWeight(weight);
    font.setLetterSpacing(QFont::AbsoluteSpacing, (pixel_size >= 32 ? -0.03 : -0.01) * pixel_size);
    return font;
}

qreal Baseline(const QFont& font, qreal top, qreal line_height) {
    const QFontMetricsF metrics{font};
    return top + (line_height - metrics.ascent() - metrics.descent()) / 2 + metrics.ascent();
}

struct Run {
    QString text;
    QFont font;
    QColor color;
};

qreal Width(const std::vector<Run>& runs) {
    qreal width = 0;
    for (const Run& run : runs) {
        width += QFontMetricsF{run.font}.horizontalAdvance(run.text);
    }
    return width;
}

void DrawRuns(QPainter& painter, qreal x, qreal baseline, const std::vector<Run>& runs) {
    for (const Run& run : runs) {
        painter.setFont(run.font);
        painter.setPen(run.color);
        painter.drawText(QPointF{x, baseline}, run.text);
        x += QFontMetricsF{run.font}.horizontalAdvance(run.text);
    }
}

QColor Mix(QColor a, QColor b, qreal t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor{static_cast<int>(a.red() + (b.red() - a.red()) * t),
                  static_cast<int>(a.green() + (b.green() - a.green()) * t),
                  static_cast<int>(a.blue() + (b.blue() - a.blue()) * t),
                  static_cast<int>(a.alpha() + (b.alpha() - a.alpha()) * t)};
}

/// Mint at 120 Hz and faster, ice at 60, amber at 45, red at 30 and slower.
QColor FrameColor(double ms, int alpha) {
    const QColor mint{134, 239, 172, alpha};
    const QColor ice{125, 211, 252, alpha};
    const QColor amber{251, 191, 36, alpha};
    const QColor spike{255, 93, 82, alpha};
    if (ms <= Budget120Ms) {
        return mint;
    }
    if (ms <= Budget60Ms) {
        return Mix(mint, ice, (ms - Budget120Ms) / (Budget60Ms - Budget120Ms));
    }
    if (ms <= Budget45Ms) {
        return Mix(ice, amber, (ms - Budget60Ms) / (Budget45Ms - Budget60Ms));
    }
    if (ms <= Budget30Ms) {
        return Mix(amber, spike, (ms - Budget45Ms) / (Budget30Ms - Budget45Ms));
    }
    return spike;
}

QColor LaneColor(std::size_t index) {
    if (index < 6) {
        return QColor{251, 191, 36};
    }
    if (index == 6) {
        return QColor{242, 133, 26};
    }
    if (index < 9) {
        return QColor{125, 211, 252};
    }
    return QColor{238, 241, 246};
}

QString Rounded(double value, int decimals) {
    return QString::number(value, 'f', decimals);
}

} // namespace

PerformanceOverlay::PerformanceOverlay(MainWindow* parent) : QWidget(parent), m_mainWindow{parent} {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    applyLayout();
    raise();

    m_motion = new QTimer(this);
    m_motion->setInterval(16);
    connect(m_motion, &QTimer::timeout, this, &PerformanceOverlay::advanceMotion);

    resetPosition(m_mainWindow->pos());
    connect(parent, &MainWindow::positionChanged, this, &PerformanceOverlay::resetPosition);
    connect(m_mainWindow, &MainWindow::statsUpdated, this, &PerformanceOverlay::updateStats);
}

PerformanceOverlay::Layout PerformanceOverlay::makeLayout() const {
    Layout layout;
    layout.detailed = !m_compact;
    layout.pad = layout.detailed ? 12 : 10;
    layout.gap = layout.detailed ? 7 : 4;
    layout.gutter = layout.detailed ? 34 : 24;
    layout.window_s = layout.detailed ? 5 : 2;
    layout.width = layout.detailed ? 460 : 320;
    const qreal header_h = layout.detailed ? 64 : 42;
    const qreal meta_h = layout.detailed ? 0 : 16;
    const qreal raster_h = layout.detailed ? 124 : 74;
    const qreal spread_h = layout.detailed ? 52 : 36;
    const qreal threads_h = layout.detailed ? 76 : 24;
    const qreal memory_h = 16;
    const qreal footer_h = 16;
    const qreal inner = layout.width - 2 * layout.pad;
    qreal y = layout.pad;
    const auto take = [&](qreal h, bool gap_after) {
        const QRectF row{layout.pad, y, inner, h};
        y += h + (gap_after ? layout.gap : 0);
        return row;
    };
    layout.header = take(header_h, true);
    if (meta_h > 0) {
        layout.meta = take(meta_h, true);
    }
    layout.raster = take(raster_h, true);
    layout.spread = take(spread_h, true);
    layout.threads = take(threads_h, true);
    layout.memory = take(memory_h, true);
    layout.footer = take(footer_h, false);
    layout.height = static_cast<int>(std::ceil(y + layout.pad));
    return layout;
}

void PerformanceOverlay::applyLayout() {
    m_compact = UISettings::values.guarana_overlay_compact.GetValue();
    const Layout layout = makeLayout();
    setFixedSize(layout.width, layout.height);
    m_frameWait = 1;
}

void PerformanceOverlay::resetPosition(const QPoint&) {
    const QPoint pos = m_mainWindow->pos();
    move(pos.x() + m_offset.x(), pos.y() + m_offset.y());
}

void PerformanceOverlay::disarmSprings() {
    m_fpsSpring.Disarm();
    m_shownSpring.Disarm();
    m_msSpring.Disarm();
    m_lowSpring.Disarm();
    m_cpuSpring.Disarm();
    m_gpuSpring.Disarm();
    for (auto& spring : m_binSpring) {
        spring.Disarm();
    }
    for (auto& spring : m_roleSpring) {
        spring.Disarm();
    }
}

QString PerformanceOverlay::limiterText() const {
    using Guarana::Overlay::Limiter;
    switch (m_limiter) {
    case Limiter::Cpu:
        return tr("cpu bound");
    case Limiter::Gpu:
        return tr("gpu bound");
    case Limiter::Pacing:
        return tr("uneven");
    case Limiter::Shaders:
        return tr("compiling");
    case Limiter::Steady:
        return tr("steady");
    case Limiter::Unknown:
        break;
    }
    return {};
}

QString PerformanceOverlay::statusText() const {
    QStringList parts;
    if (m_shadersBuilding > 0) {
        parts << tr("%n shader(s)", "", m_shadersBuilding);
    }
    if (!m_frameGenOn) {
        parts << tr("FG off");
    } else if (m_generatedFps >= 0.5) {
        parts << tr("FG on +%1").arg(std::round(m_generatedFps), 0, 'f', 0);
    } else {
        parts << tr("FG on");
    }
    if (!m_tags.isEmpty()) {
        parts << m_tags;
    }
    return parts.join(QStringLiteral("  "));
}

void PerformanceOverlay::updateStats(const Core::PerfStatsResults& results,
                                     const VideoCore::ShaderNotify&) {
    auto& system = *QtCommon::system;
    system.GetPerfStats().SetFrameTimeHistoryEnabled(isVisible());
    if (!isVisible()) {
        return;
    }

    m_fps.reset();
    if (std::isfinite(results.average_game_fps)) {
        m_fps = results.average_game_fps;
        m_fpsSamples.push_back(static_cast<float>(std::round(*m_fps)));
        while (m_fpsSamples.size() > FpsSamples) {
            m_fpsSamples.pop_front();
        }
    }
    m_speed = std::isfinite(results.emulation_speed) ? results.emulation_speed : 0;
    m_shadersBuilding = m_mainWindow->ShadersBuilding();
    m_generatedFps = m_mainWindow->FrameGenFps();
    m_frameGenOn = Settings::values.frame_gen.GetValue();

    QStringList tags;
    if (const QString& dlss = m_mainWindow->DlssStatusText(); !dlss.isEmpty()) {
        tags << dlss;
    }
    // Ocarina of Time's upscaler, as the status bar shows it: visible in fullscreen too.
    if (const QString upscaler = m_mainWindow->UpscaleStatusText(); !upscaler.isEmpty()) {
        tags << upscaler;
    }
    switch (Settings::values.scaling_filter.GetValue()) {
    case Settings::ScalingFilter::Fsr:
        tags << QStringLiteral("FSR");
        break;
    case Settings::ScalingFilter::Fsr3:
        tags << QStringLiteral("FSR 3");
        break;
    case Settings::ScalingFilter::Sgsr:
    case Settings::ScalingFilter::SgsrEdge:
        tags << QStringLiteral("SGSR");
        break;
    default:
        break;
    }
    if (const auto& scaling = Settings::values.resolution_info; scaling.active) {
        tags << QStringLiteral("%1×").arg(scaling.up_factor, 0, 'g', 3);
    }
    m_tags = tags.join(QStringLiteral("  "));

    auto& renderer = system.GPU().Renderer();
    renderer.RequestGpuBusySample();
    m_gpuBusyMs = renderer.GetGpuBusyMs();

    // Every 2 s: on Windows the process query also reads the whole system's counters.
    const auto now = std::chrono::steady_clock::now();
    if (!m_memorySampled || now - *m_memorySampled >= std::chrono::seconds{2}) {
        m_memorySampled = now;
        sampleMemory();
    }

    if (m_compact != UISettings::values.guarana_overlay_compact.GetValue()) {
        applyLayout();
    }
    sampleLive(makeLayout().window_s, true);
    if (m_fps) {
        const double shown = *m_fps + (m_generatedFps >= 0.5 ? m_generatedFps : 0);
        m_fpsSpring.Step(*m_fps, 0);
        m_shownSpring.Step(shown, 0);
    }
    update();
}

void PerformanceOverlay::sampleMemory() {
    const auto available = Common::GetAvailablePhysicalMemory();
    m_ram = Guarana::Overlay::RamFigure(Common::GetProcessMemoryUsage().working_set, available);
    m_vram.reset();
    m_vramShared = false;
    if (!QtCommon::system || !QtCommon::system->IsPoweredOn()) {
        return;
    }
    if (const auto usage = QtCommon::system->GPU().Renderer().GetVideoMemoryUsage()) {
        m_vram = Guarana::Overlay::VramFigure(usage->used, usage->budget, usage->shared, available);
        m_vramShared = usage->shared;
    }
}

void PerformanceOverlay::sampleLive(double window_s, bool force) {
    if (QtCommon::system && QtCommon::system->IsPoweredOn() && (force || m_frameWait >= 0.05)) {
        // The present history is copied under the emulator lock. Pull it at 20 Hz; the trace still
        // scrolls every paint from the timestamps already in hand.
        m_frameWait = 0;
        const auto window = std::chrono::duration_cast<Core::FrameTimeHistory::Clock::duration>(
            std::chrono::duration<double>(window_s));
        m_frames = QtCommon::system->GetPerfStats().GetRecentFrameTimes(window);
    }

    const auto now = Core::FrameTimeHistory::Clock::now();
    m_msScratch.clear();
    m_msScratch.reserve(m_frames.size());
    double recent_sum = 0;
    int recent_n = 0;
    double all_sum = 0;
    const Core::FrameTimeHistory::Entry* newest = nullptr;
    double newest_age = 0;
    for (const auto& frame : m_frames) {
        const double age = std::chrono::duration<double>(now - frame.end).count();
        if (age < -0.05 || age > window_s || !(frame.ms > 0.f)) {
            continue;
        }
        m_msScratch.push_back(frame.ms);
        all_sum += frame.ms;
        if (age <= 0.35) {
            recent_sum += frame.ms;
            ++recent_n;
        }
        if (newest == nullptr || age < newest_age) {
            newest = &frame;
            newest_age = age;
        }
    }
    if (recent_n > 0) {
        m_frameMs = recent_sum / recent_n;
    } else if (!m_msScratch.empty()) {
        m_frameMs = all_sum / static_cast<double>(m_msScratch.size());
    } else {
        m_frameMs.reset();
    }
    if (const auto low = Guarana::Overlay::LowPercent(m_msScratch)) {
        m_lowFps = low->fps;
        m_lowMs = low->ms;
    } else {
        m_lowFps.reset();
        m_lowMs.reset();
    }
    const auto pacing = Guarana::Overlay::MeasurePacing(m_msScratch);
    m_bins = Guarana::Overlay::BuildHistogram(m_msScratch).counts;
    m_hitch = newest != nullptr && pacing &&
              Guarana::Overlay::NewestIsHitch(newest->ms, pacing->median_ms, newest_age);

    if (QtCommon::system && QtCommon::system->IsPoweredOn() &&
        (m_cpuWait >= 0.1 || !m_cpuSnapshot)) {
        m_cpuWait = 0;
        const auto snap = Common::DkbCpuTrace::ReadSnapshot();
        if (m_cpuSnapshot) {
            const double wall_ns =
                std::chrono::duration<double, std::nano>(snap.wall_time - m_cpuSnapshot->wall_time)
                    .count();
            double busiest = 0;
            bool any = false;
            for (std::size_t i = 0; i < RoleCount; ++i) {
                const auto delta =
                    Common::DkbCpuTrace::CpuTimeDelta(m_cpuSnapshot->threads[i], snap.threads[i]);
                const std::optional<double> cpu =
                    delta ? std::optional<double>{static_cast<double>(*delta)} : std::nullopt;
                const auto load = Guarana::Overlay::PercentOfWall(cpu, wall_ns);
                m_roleKnown[i] = load.known;
                m_roleTarget[i] = load.percent;
                if (load.known && static_cast<Common::DkbCpuTrace::Role>(i) !=
                                      Common::DkbCpuTrace::Role::VulkanWorker) {
                    busiest = std::max(busiest, load.percent);
                    any = true;
                }
            }
            if (any) {
                m_cpuPercent = busiest;
            }
        }
        m_cpuSnapshot = snap;
    }

    if (!m_frameMs) {
        m_gpuPercent.reset();
    } else if (m_gpuBusyMs && *m_frameMs > 0) {
        m_gpuPercent = std::min(100.0, *m_gpuBusyMs / *m_frameMs * 100.0);
    }
    m_limiter = Guarana::Overlay::Classify(m_shadersBuilding, pacing, m_cpuPercent, m_gpuPercent);
    if (m_frameMs) {
        m_msSpring.Step(*m_frameMs, 0);
    }
    if (m_lowFps) {
        m_lowSpring.Step(*m_lowFps, 0);
    }
    if (m_cpuPercent) {
        m_cpuSpring.Step(*m_cpuPercent, 0);
    }
    if (m_gpuPercent) {
        m_gpuSpring.Step(*m_gpuPercent, 0);
    }
}

void PerformanceOverlay::advanceMotion() {
    if (!isVisible()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    double dt = 0.016;
    if (m_tickStarted) {
        dt = std::clamp(std::chrono::duration<double>(now - m_tick).count(), 0.0, 0.05);
    }
    m_tick = now;
    m_tickStarted = true;
    m_phase += dt / 3.2;
    if (m_phase >= 1.0) {
        m_phase -= std::floor(m_phase);
    }
    m_frameWait += dt;
    m_cpuWait += dt;

    if (m_compact != UISettings::values.guarana_overlay_compact.GetValue()) {
        applyLayout();
    }
    sampleLive(makeLayout().window_s, false);

    if (m_hitch && !m_hitchLatch) {
        m_flash = 1;
    }
    m_hitchLatch = m_hitch;
    m_flash *= std::exp(-4.0 * dt);
    if (m_flash < 0.01) {
        m_flash = 0;
    }

    if (m_fps) {
        const double shown = *m_fps + (m_generatedFps >= 0.5 ? m_generatedFps : 0);
        m_fpsSpring.Step(*m_fps, dt);
        m_shownSpring.Step(shown, dt);
    }
    if (m_frameMs) {
        m_msSpring.Step(*m_frameMs, dt);
    }
    if (m_lowFps) {
        m_lowSpring.Step(*m_lowFps, dt);
    }
    if (m_cpuPercent) {
        m_cpuSpring.Step(*m_cpuPercent, dt);
    }
    if (m_gpuPercent) {
        m_gpuSpring.Step(*m_gpuPercent, dt);
    }
    const int bin_max = *std::ranges::max_element(m_bins);
    for (int i = 0; i < HistogramBins; ++i) {
        const double target = bin_max > 0
                                  ? static_cast<double>(m_bins[static_cast<std::size_t>(i)]) /
                                        static_cast<double>(bin_max)
                                  : 0;
        m_binSpring[static_cast<std::size_t>(i)].Step(target, dt);
    }
    for (std::size_t i = 0; i < RoleCount; ++i) {
        m_roleSpring[i].Step(m_roleKnown[i] ? m_roleTarget[i] : 0, dt);
    }
    update();
}

void PerformanceOverlay::paintChrome(QPainter& painter) const {
    const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient bg(bounds.topLeft(), bounds.bottomLeft());
    bg.setColorAt(0, QColor{12, 18, 32, 232});
    bg.setColorAt(1, QColor{7, 11, 20, 240});
    const qreal heat = std::max(m_flash, m_hitch ? 0.45 : 0.0);
    QColor border{242, 133, 26, 90};
    if (heat > 0) {
        border = Mix(border, QColor{255, 93, 82, 210}, heat);
    }
    painter.setPen(QPen{border, 1});
    painter.setBrush(bg);
    painter.drawRoundedRect(bounds, 10, 10);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{255, 255, 255, 18});
    painter.drawRect(QRectF{bounds.left() + 10, bounds.top() + 1, bounds.width() * 0.42, 1});
    if (heat > 0) {
        painter.setPen(QPen{QColor{255, 93, 82, static_cast<int>(160 * heat)}, 1.25});
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(bounds.adjusted(2, 2, -2, -2), 8, 8);
    }
}

void PerformanceOverlay::paintHeader(QPainter& painter, const Layout& layout) const {
    const QRectF area = layout.header;
    const QColor text{238, 241, 246};
    const QColor muted{143, 153, 173};
    const QColor amber{251, 191, 36};
    const QColor spike{255, 93, 82};
    const qreal heat = std::max(m_flash, m_hitch ? 0.35 : 0.0);
    const QColor fps_color = heat > 0 ? Mix(text, spike, heat) : text;
    const bool shown_hot = m_generatedFps >= 0.5;
    const bool low_sag = m_fps && m_lowFps && m_lowSpring.y < m_fpsSpring.y * 0.8;
    const QFont hero = MonoFont(layout.detailed ? 40 : 32, QFont::DemiBold);
    const QFont small_font = MonoFont(layout.detailed ? 13 : 12);
    const QFont small_bold_font = MonoFont(layout.detailed ? 13 : 12, QFont::DemiBold);
    const qreal hero_box = layout.detailed ? 44 : area.height();
    const qreal hero_base = Baseline(hero, area.top(), hero_box);
    const QString fps = m_fps ? Rounded(std::round(m_fpsSpring.y), 0) : Dash();
    const std::vector<Run> fps_runs{{fps, hero, fps_color}};
    DrawRuns(painter, area.left(), hero_base, fps_runs);
    DrawRuns(painter, area.left() + Width(fps_runs) + 6, hero_base,
             {{QStringLiteral("fps"), small_font, muted}});

    if (layout.detailed && m_frameMs) {
        DrawRuns(painter, area.left(), Baseline(small_font, area.top() + 44, 16),
                 {{QStringLiteral("%1 ms").arg(m_msSpring.y, 0, 'f', 1), small_font, text}});
    } else if (layout.detailed) {
        DrawRuns(painter, area.left(), Baseline(small_font, area.top() + 44, 16),
                 {{Dash(), small_font, muted}});
    }

    const qreal right = area.right() - 2;
    const QString shown = m_fps ? Rounded(std::round(m_shownSpring.y), 0) : Dash();
    const std::vector<Run> shown_runs{
        {shown, small_bold_font, shown_hot ? amber : text},
        {QStringLiteral(" shown"), small_font, muted},
    };
    if (!layout.detailed) {
        DrawRuns(painter, right - Width(shown_runs), hero_base, shown_runs);
        return;
    }

    const QString low = m_lowFps ? Rounded(std::round(m_lowSpring.y), 0) : Dash();
    const std::vector<Run> low_runs{
        {QStringLiteral("1% "), small_font, muted},
        {low, small_bold_font, low_sag ? amber : text},
    };
    const QString speed = m_speed > 0 ? QStringLiteral("%1×").arg(m_speed, 0, 'f', 2) : Dash();
    QColor speed_color = muted;
    if (m_speed > 1.05) {
        speed_color = amber;
    } else if (m_speed > 0 && m_speed < 0.92) {
        speed_color = spike;
    }
    QColor limit_color = muted;
    using Guarana::Overlay::Limiter;
    if (m_limiter == Limiter::Pacing || m_limiter == Limiter::Shaders) {
        limit_color = spike;
    } else if (m_limiter == Limiter::Cpu || m_limiter == Limiter::Gpu) {
        limit_color = amber;
    }
    const QString limit = limiterText();
    std::vector<Run> pace_runs{{speed, small_font, speed_color}};
    if (!limit.isEmpty()) {
        pace_runs.push_back({QStringLiteral("  "), small_font, muted});
        pace_runs.push_back({limit, small_font, limit_color});
    }
    const qreal line = 18;
    DrawRuns(painter, right - Width(shown_runs), Baseline(small_font, area.top() + 2, line), shown_runs);
    DrawRuns(painter, right - Width(low_runs), Baseline(small_font, area.top() + 2 + line, line),
             low_runs);
    DrawRuns(painter, right - Width(pace_runs), Baseline(small_font, area.top() + 2 + line * 2, line),
             pace_runs);
}

void PerformanceOverlay::paintMeta(QPainter& painter, const Layout& layout) const {
    if (layout.meta.width() <= 0) {
        return;
    }
    const QColor text{238, 241, 246};
    const QColor muted{143, 153, 173};
    const QColor amber{251, 191, 36};
    const QColor spike{255, 93, 82};
    const QFont font = MonoFont(12);
    const QFont bold = MonoFont(12, QFont::DemiBold);
    const qreal baseline = Baseline(font, layout.meta.top(), layout.meta.height());
    const bool low_sag = m_fps && m_lowFps && m_lowSpring.y < m_fpsSpring.y * 0.8;
    QColor speed_color = muted;
    if (m_speed > 1.05) {
        speed_color = amber;
    } else if (m_speed > 0 && m_speed < 0.92) {
        speed_color = spike;
    }
    using Guarana::Overlay::Limiter;
    QColor limit_color = muted;
    if (m_limiter == Limiter::Pacing || m_limiter == Limiter::Shaders) {
        limit_color = spike;
    } else if (m_limiter == Limiter::Cpu || m_limiter == Limiter::Gpu) {
        limit_color = amber;
    }
    const QString ms = m_frameMs ? QStringLiteral("%1 ms").arg(m_msSpring.y, 0, 'f', 1) : Dash();
    const QString speed = m_speed > 0 ? QStringLiteral("%1×").arg(m_speed, 0, 'f', 2) : Dash();
    std::vector<Run> runs{
        {QStringLiteral("1% "), font, muted},
        {m_lowFps ? Rounded(std::round(m_lowSpring.y), 0) : Dash(), bold, low_sag ? amber : text},
        {QStringLiteral("    "), font, muted},
        {ms, font, text},
        {QStringLiteral("    "), font, muted},
        {speed, font, speed_color},
    };
    if (const QString limit = limiterText(); !limit.isEmpty()) {
        runs.push_back({QStringLiteral("    "), font, muted});
        runs.push_back({limit, font, limit_color});
    }
    DrawRuns(painter, layout.meta.left(), baseline, runs);
}

void PerformanceOverlay::paintRaster(QPainter& painter, const Layout& layout) const {
    const QRectF plot = layout.raster.adjusted(0, 0, -layout.gutter, 0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{4, 7, 14, 220});
    painter.drawRoundedRect(plot, 6, 6);

    const auto now = Core::FrameTimeHistory::Clock::now();
    float peak = 0;
    for (const auto& frame : m_frames) {
        const double age = std::chrono::duration<double>(now - frame.end).count();
        if (age < -0.05 || age > layout.window_s) {
            continue;
        }
        peak = std::max(peak, frame.ms);
    }
    const double scale = Guarana::Overlay::ScaleFromPeak(peak);
    const auto y_of = [&](double ms) {
        return plot.bottom() - std::clamp(ms / scale, 0.0, 1.0) * plot.height();
    };

    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(plot.adjusted(1, 1, -1, -1), 5, 5);
    painter.setClipPath(clip);

    const qreal y60 = y_of(Budget60Ms);
    painter.fillRect(QRectF{plot.left(), y60, plot.width(), plot.bottom() - y60},
                     QColor{125, 211, 252, 18});

    struct Head {
        qreal x = 0;
        qreal w = 0;
        qreal h = 0;
        float ms = 0;
        bool drew = false;
    } head;
    for (const auto& frame : m_frames) {
        const double age = std::chrono::duration<double>(now - frame.end).count();
        const auto mark = Guarana::Overlay::MarkInterval(age, frame.ms, layout.window_s, scale);
        if (!mark.visible) {
            continue;
        }
        const qreal x = plot.left() + mark.x0 * plot.width();
        const qreal w = std::max<qreal>(1, (mark.x1 - mark.x0) * plot.width());
        const qreal h = std::max<qreal>(1, mark.height * plot.height());
        const double age_t = std::clamp(age / layout.window_s, 0.0, 1.0);
        const int alpha = static_cast<int>(235 - 55 * age_t);
        painter.fillRect(QRectF{x, plot.bottom() - h, w, h}, FrameColor(frame.ms, alpha));
        head = {x, w, h, frame.ms, true};
    }

    double frac = std::fmod(std::chrono::duration<double>(now.time_since_epoch()).count(), 1.0);
    if (frac < 0) {
        frac += 1;
    }
    for (double age = frac; age < layout.window_s; age += 1) {
        if (age < 0.04) {
            continue;
        }
        const qreal x = plot.right() - (age / layout.window_s) * plot.width();
        painter.fillRect(QRectF{x, plot.top(), 1, plot.height()}, QColor{255, 255, 255, 28});
    }

    struct Guide {
        double ms;
        const char* label;
        bool primary;
    };
    const Guide guides[] = {
        {Budget60Ms, "60", true},
        {Budget30Ms, "30", false},
        {Budget120Ms, "120", false},
    };
    for (const Guide& guide : guides) {
        if (guide.ms >= scale) {
            continue;
        }
        const QColor ink = guide.primary ? QColor{251, 191, 36, 150} : QColor{255, 255, 255, 50};
        painter.fillRect(QRectF{plot.left(), y_of(guide.ms), plot.width(), 1}, ink);
    }

    if (m_lowMs && *m_lowMs > 0 && *m_lowMs < scale) {
        const qreal y = y_of(*m_lowMs);
        QPen pen{QColor{251, 191, 36, 120}, 1};
        pen.setDashPattern({2, 3});
        painter.setPen(pen);
        painter.drawLine(QPointF{plot.left(), y}, QPointF{plot.right(), y});
        painter.setPen(Qt::NoPen);
    }

    painter.setPen(Qt::NoPen);
    if (head.drew) {
        const qreal pulse = 0.5 + 0.5 * std::sin(m_phase * 6.28318530718);
        painter.setBrush(FrameColor(head.ms, static_cast<int>(70 + 50 * pulse)));
        painter.drawEllipse(QPointF{head.x + head.w * 0.5, plot.bottom() - head.h}, 3.4, 3.4);
    } else {
        painter.setPen(QColor{143, 153, 173});
        painter.setFont(MonoFont(12));
        painter.drawText(plot, Qt::AlignCenter, tr("waiting"));
    }
    painter.restore();

    // Budget labels in the gutter. 60 is listed first, so it wins when the lines bunch up.
    std::array<qreal, 3> drawn_y{};
    int drawn = 0;
    const QFont guide_font = MonoFont(10);
    for (const Guide& guide : guides) {
        if (guide.ms >= scale) {
            continue;
        }
        const qreal y = y_of(guide.ms);
        bool collide = false;
        for (int i = 0; i < drawn; ++i) {
            if (std::abs(drawn_y[static_cast<std::size_t>(i)] - y) < 12) {
                collide = true;
                break;
            }
        }
        if (collide) {
            continue;
        }
        drawn_y[static_cast<std::size_t>(drawn++)] = y;
        const QColor label = guide.primary ? QColor{238, 241, 246} : QColor{143, 153, 173};
        const qreal top = std::clamp(y - 6, plot.top(), plot.bottom() - 12);
        DrawRuns(painter, plot.right() + 6, Baseline(guide_font, top, 12),
                 {{QString::fromLatin1(guide.label), guide_font, label}});
    }
}

void PerformanceOverlay::paintSpread(QPainter& painter, const Layout& layout) const {
    constexpr int n = HistogramBins;
    const qreal label_h = 13;
    const qreal gap = 4;
    const QRectF area = layout.spread;
    const qreal bar_w = (area.width() - gap * (n - 1)) / n;
    const qreal bar_h = area.height() - label_h;
    const QFont font = MonoFont(10);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < n; ++i) {
        const qreal x = area.left() + i * (bar_w + gap);
        painter.setBrush(QColor{255, 255, 255, 16});
        painter.drawRoundedRect(QRectF{x, area.top() + bar_h - 1, bar_w, 1}, 1, 1);
        const double spring = std::clamp(m_binSpring[static_cast<std::size_t>(i)].y, 0.0, 1.0);
        if (spring > 0.02) {
            const qreal h = std::max<qreal>(2, spring * (bar_h - 2));
            painter.setBrush(FrameColor(BinToneMs[static_cast<std::size_t>(i)], 220));
            painter.drawRoundedRect(QRectF{x, area.top() + bar_h - h, bar_w, h}, 2, 2);
        }
        const bool anchor = i == 2;
        const QColor ink = anchor ? QColor{238, 241, 246} : QColor{107, 117, 137};
        const QString label = QString::fromLatin1(BinLabels[static_cast<std::size_t>(i)]);
        const qreal text_w = QFontMetricsF{font}.horizontalAdvance(label);
        DrawRuns(painter, x + (bar_w - text_w) / 2, Baseline(font, area.top() + bar_h, label_h),
                 {{label, font, ink}});
    }
}

void PerformanceOverlay::paintThreads(QPainter& painter, const Layout& layout) const {
    const QRectF area = layout.threads;
    const QFont font = MonoFont(11);
    const QFont small_font = MonoFont(10);
    const QColor muted{143, 153, 173};
    const QColor dim{107, 117, 137};
    const QColor text{238, 241, 246};
    const QString cpu =
        m_cpuPercent ? Rounded(std::round(m_cpuSpring.y), 0) + QStringLiteral("%") : Dash();
    const QString gpu =
        m_gpuPercent ? Rounded(std::round(m_gpuSpring.y), 0) + QStringLiteral("%") : Dash();

    if (!layout.detailed) {
        const QString cpu_text = QStringLiteral("CPU ") + cpu;
        const QString gpu_text = QStringLiteral("GPU ") + gpu;
        const qreal cpu_w = QFontMetricsF{font}.horizontalAdvance(cpu_text);
        const qreal gpu_w = QFontMetricsF{font}.horizontalAdvance(gpu_text);
        qreal cell = 9;
        qreal cell_gap = 2;
        qreal cells_w = RoleCount * cell + (RoleCount - 1) * cell_gap;
        qreal bar_w = area.width() - cpu_w - gpu_w - cells_w - 22;
        if (bar_w < 28) {
            cell = 7;
            cells_w = RoleCount * cell + (RoleCount - 1) * cell_gap;
            bar_w = area.width() - cpu_w - gpu_w - cells_w - 22;
        }
        const qreal base = Baseline(font, area.top(), area.height());
        DrawRuns(painter, area.left(), base, {{cpu_text, font, text}});
        const qreal cells_x = area.left() + cpu_w + 8;
        const qreal cell_h = 10;
        const qreal cell_y = area.center().y() - cell_h / 2;
        painter.setPen(Qt::NoPen);
        for (std::size_t i = 0; i < RoleCount; ++i) {
            const qreal x = cells_x + static_cast<qreal>(i) * (cell + cell_gap);
            painter.setBrush(QColor{255, 255, 255, 16});
            painter.drawRoundedRect(QRectF{x, cell_y, cell, cell_h}, 2, 2);
            if (m_roleKnown[i]) {
                const double pct = std::clamp(m_roleSpring[i].y, 0.0, 100.0);
                QColor ink = LaneColor(i);
                ink.setAlpha(static_cast<int>(40 + pct / 100.0 * 200));
                painter.setBrush(ink);
                painter.drawRoundedRect(QRectF{x, cell_y, cell, cell_h}, 2, 2);
            }
        }
        const qreal gpu_x = cells_x + cells_w + 8;
        DrawRuns(painter, gpu_x, base, {{gpu_text, font, text}});
        if (bar_w > 8) {
            const QRectF bar{gpu_x + gpu_w + 6, area.center().y() - 3, bar_w, 6};
            painter.setBrush(QColor{255, 255, 255, 18});
            painter.drawRoundedRect(bar, 2, 2);
            if (m_gpuPercent) {
                const qreal fill = bar.width() * std::clamp(m_gpuSpring.y, 0.0, 100.0) / 100.0;
                painter.setBrush(QColor{125, 211, 252});
                painter.drawRoundedRect(QRectF{bar.left(), bar.top(), fill, bar.height()}, 2, 2);
            }
        }
        return;
    }

    const QRectF card{area.right() - 88, area.top(), 88, area.height()};
    const QRectF lanes{area.left(), area.top(), card.left() - area.left() - 10, area.height()};
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{255, 255, 255, 12});
    painter.drawRoundedRect(card, 6, 6);
    DrawRuns(painter, card.left() + 8, Baseline(small_font, card.top() + 6, 12),
             {{QStringLiteral("GPU"), small_font, muted}});
    const QFont gpu_font = MonoFont(18, QFont::DemiBold);
    DrawRuns(painter, card.left() + 8, Baseline(gpu_font, card.top() + 20, 24),
             {{gpu, gpu_font, text}});
    const QRectF gpu_bar{card.left() + 8, card.bottom() - 14, card.width() - 16, 6};
    painter.setBrush(QColor{255, 255, 255, 18});
    painter.drawRoundedRect(gpu_bar, 2, 2);
    if (m_gpuPercent) {
        const qreal fill = gpu_bar.width() * std::clamp(m_gpuSpring.y, 0.0, 100.0) / 100.0;
        painter.setBrush(QColor{125, 211, 252});
        painter.drawRoundedRect(QRectF{gpu_bar.left(), gpu_bar.top(), fill, gpu_bar.height()}, 2,
                                2);
    }

    const QString cpu_label = QStringLiteral("CPU ") + cpu;
    DrawRuns(painter, lanes.left(), Baseline(font, lanes.top(), 14), {{cpu_label, font, text}});
    constexpr qreal title_h = 16;
    constexpr qreal name_h = 14;
    constexpr qreal group = 14;
    const QRectF tracks{lanes.left(), lanes.top() + title_h, lanes.width(),
                        lanes.height() - title_h - name_h};
    const qreal slot = (tracks.width() - group) / static_cast<qreal>(RoleCount);
    qreal x = tracks.left();
    for (std::size_t i = 0; i < RoleCount; ++i) {
        if (i == 7) {
            painter.fillRect(QRectF{x + group * 0.5, tracks.top(), 1, tracks.height()},
                             QColor{255, 255, 255, 28});
            x += group;
        }
        const QRectF track{x + 2, tracks.top(), std::max<qreal>(3, slot - 5), tracks.height()};
        painter.setBrush(QColor{255, 255, 255, 14});
        painter.drawRoundedRect(track, 2, 2);
        if (m_roleKnown[i]) {
            const double pct = std::clamp(m_roleSpring[i].y, 0.0, 100.0);
            const qreal fill_h = track.height() * pct / 100.0;
            if (fill_h > 0.5) {
                QColor ink = LaneColor(i);
                ink.setAlpha(230);
                painter.setBrush(ink);
                painter.drawRoundedRect(
                    QRectF{track.left(), track.bottom() - fill_h, track.width(), fill_h}, 2, 2);
            }
        }
        const QString name = QString::fromLatin1(LaneLabels[i]);
        const QFont name_font = MonoFont(9);
        const qreal name_w = QFontMetricsF{name_font}.horizontalAdvance(name);
        DrawRuns(painter, track.center().x() - name_w / 2,
                 Baseline(name_font, tracks.bottom(), name_h),
                 {{name, name_font, m_roleKnown[i] ? muted : dim}});
        x += slot;
    }
}

void PerformanceOverlay::paintMemory(QPainter& painter, const Layout& layout) const {
    const QRectF area = layout.memory;
    const QFont font = MonoFont(layout.detailed ? 11 : 10);
    const QFont bold = MonoFont(layout.detailed ? 11 : 10, QFont::DemiBold);
    const QColor text{238, 241, 246};
    const QColor muted{143, 153, 173};
    const QColor dim{107, 117, 137};
    const QColor amber{251, 191, 36};
    // What this process uses, then the room left, amber when nearly out (Fritchii's rows).
    const auto runs_of = [&](const QString& label,
                             const std::optional<Guarana::Overlay::MemoryFigure>& figure,
                             bool shared, bool with_room) {
        std::vector<Run> runs{{label + QStringLiteral(" "), font, muted}};
        if (!figure) {
            runs.push_back({Dash(), font, dim});
            return runs;
        }
        runs.push_back({tr("%1 GB").arg(figure->used_gib, 0, 'f', 1), bold, text});
        if (shared && layout.detailed) {
            runs.push_back({tr(" shared"), font, dim});
        }
        if (with_room && figure->free_gib) {
            runs.push_back({QStringLiteral("  "), font, muted});
            runs.push_back({tr("%1 free").arg(*figure->free_gib, 0, 'f', 1), font,
                            figure->low ? amber : muted});
        }
        return runs;
    };
    std::vector<Run> ram = runs_of(QStringLiteral("RAM"), m_ram, false, true);
    std::vector<Run> vram = runs_of(QStringLiteral("VRAM"), m_vram, m_vramShared, true);
    if (Width(ram) + Width(vram) + 12 > area.width()) {
        ram = runs_of(QStringLiteral("RAM"), m_ram, false, false);
        vram = runs_of(QStringLiteral("VRAM"), m_vram, m_vramShared, false);
    }
    const qreal baseline = Baseline(font, area.top(), area.height());
    DrawRuns(painter, area.left(), baseline, ram);
    DrawRuns(painter, area.right() - Width(vram), baseline, vram);
}

void PerformanceOverlay::paintFooter(QPainter& painter, const Layout& layout) const {
    const QFont font = MonoFont(11);
    const QColor muted{143, 153, 173};
    const QColor dim{107, 117, 137};
    const qreal baseline = Baseline(font, layout.footer.top(), layout.footer.height());
    const QString status = statusText();
    if (!layout.detailed) {
        const QString elided =
            QFontMetricsF{font}.elidedText(status, Qt::ElideRight, layout.footer.width());
        DrawRuns(painter, layout.footer.left(), baseline, {{elided, font, muted}});
        return;
    }

    QString range = QStringLiteral("—  —  —");
    const std::vector<float> samples(m_fpsSamples.begin(), m_fpsSamples.end());
    if (const auto summary = Core::Summarize(samples)) {
        range = QStringLiteral("%1  %2  %3")
                    .arg(summary->min_ms, 0, 'f', 0)
                    .arg(std::round(summary->median_ms), 0, 'f', 0)
                    .arg(summary->max_ms, 0, 'f', 0);
    }
    const std::vector<Run> range_runs{
        {range, font, muted},
        {QStringLiteral(" fps"), font, dim},
    };
    DrawRuns(painter, layout.footer.left(), baseline, range_runs);
    const qreal room = layout.footer.width() - Width(range_runs) - 12;
    const QString elided =
        QFontMetricsF{font}.elidedText(status, Qt::ElideRight, std::max<qreal>(0, room));
    const qreal status_w = QFontMetricsF{font}.horizontalAdvance(elided);
    DrawRuns(painter, layout.footer.right() - status_w, baseline, {{elided, font, muted}});
}

void PerformanceOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const Layout layout = makeLayout();
    paintChrome(painter);
    paintHeader(painter, layout);
    paintMeta(painter, layout);
    paintRaster(painter, layout);
    paintSpread(painter, layout);
    paintThreads(painter, layout);
    paintMemory(painter, layout);
    paintFooter(painter, layout);
}

void PerformanceOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_tickStarted = false;
    m_frameWait = 1;
    m_cpuWait = 1;
    if (QtCommon::system && QtCommon::system->IsPoweredOn()) {
        QtCommon::system->GetPerfStats().SetFrameTimeHistoryEnabled(true);
        QtCommon::system->GPU().Renderer().RequestGpuBusySample();
    }
    if (m_motion && !m_motion->isActive()) {
        m_motion->start();
    }
}

void PerformanceOverlay::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_motion) {
        m_motion->stop();
    }
    m_tickStarted = false;
    disarmSprings();
    m_flash = 0;
    m_hitch = false;
    m_hitchLatch = false;
    m_phase = 0;
    m_frameWait = 1;
    m_cpuWait = 1;
    if (QtCommon::system && QtCommon::system->IsPoweredOn()) {
        QtCommon::system->GetPerfStats().SetFrameTimeHistoryEnabled(false);
    }
    m_fps.reset();
    m_fpsSamples.clear();
    m_frameMs.reset();
    m_cpuPercent.reset();
    m_gpuPercent.reset();
    m_lowFps.reset();
    m_lowMs.reset();
    m_gpuBusyMs.reset();
    m_ram.reset();
    m_vram.reset();
    m_vramShared = false;
    m_memorySampled.reset();
    m_cpuSnapshot.reset();
    m_frames.clear();
    m_msScratch.clear();
    m_bins.fill(0);
    m_roleKnown.fill(false);
    m_roleTarget.fill(0);
    m_limiter = Guarana::Overlay::Limiter::Unknown;
    m_tags.clear();
    m_shadersBuilding = 0;
    m_generatedFps = 0;
    m_speed = 0;
}

void PerformanceOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) && windowHandle() &&
        windowHandle()->startSystemMove()) {
        return;
    }
    m_drag_start_pos = event->pos();
}

void PerformanceOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        const QPoint new_global_pos = event->globalPosition().toPoint() - m_drag_start_pos;
        m_offset = new_global_pos - m_mainWindow->pos();
        move(new_global_pos);
    }
}

void PerformanceOverlay::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QAction* detailed = menu.addAction(tr("Detailed trace"));
    detailed->setCheckable(true);
    detailed->setChecked(!m_compact);
    if (menu.exec(event->globalPos()) != detailed) {
        return;
    }
    UISettings::values.guarana_overlay_compact = !detailed->isChecked();
    applyLayout();
}

void PerformanceOverlay::closeEvent(QCloseEvent* event) {
    emit closed();
    QWidget::closeEvent(event);
}
