// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package io.github.zortos293.lichii.utils

import android.content.Context
import android.os.Build
import android.os.PerformanceHintManager
import android.os.PowerManager
import androidx.annotation.RequiresApi
import io.github.zortos293.lichii.NativeLibrary

/**
 * CPU hint session and thermal backoff for a playing session.
 *
 * The hint target is one 120 Hz frame, 8.33 ms ([PREFERRED_UPDATE_RATE_NS]). Each displayed
 * frame reports its duration through [onFramePresented]. Thermal status uses
 * [PowerManager.addThermalStatusListener] (API 29). The platform has no app-facing
 * ThermalManager; that listener is the thermal API. A status of LIGHT or higher is throttling:
 * the session target drops to 60 Hz, or 30 Hz at SEVERE, and API 35 asks for power efficiency.
 *
 * [PerformanceHintManager] is only touched from [HintSession], and [HintSession] is only entered
 * from a method that runs when [Build.VERSION.SDK_INT] is at least 33, so older devices never
 * resolve that class.
 */
object AdpfBooster {
    /** 1/120 s. Passed to createHintSession as the target work duration. */
    const val PREFERRED_UPDATE_RATE_NS = 8_333_333L

    private const val TARGET_60_NS = 16_666_667L
    private const val TARGET_30_NS = 33_333_333L

    private var appContext: Context? = null
    private var running = false
    private var targetWorkNs = PREFERRED_UPDATE_RATE_NS
    private var preferEfficiency = false
    // Any, not the API 29 listener type: this object is loaded on minSdk 24.
    private var thermalListener: Any? = null
    private var thermalStatus = 0

    @Synchronized
    fun start(context: Context) {
        appContext = context.applicationContext
        running = true
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            registerThermalListener()
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            ensureHintSession()
        }
    }

    @Synchronized
    fun stop() {
        running = false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            closeHintSession()
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            unregisterThermalListener()
        }
        appContext = null
        thermalStatus = 0
        targetWorkNs = PREFERRED_UPDATE_RATE_NS
        preferEfficiency = false
    }

    /** Called on the present thread once per displayed frame. */
    @Synchronized
    fun onFramePresented(workDurationNs: Long) {
        if (!running || Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return
        }
        reportFrame(workDurationNs)
    }

    @Synchronized
    private fun onThermalStatus(status: Int) {
        val changed = status != thermalStatus
        thermalStatus = status
        if (!running) {
            return
        }
        val throttling = status >= PowerManager.THERMAL_STATUS_LIGHT
        targetWorkNs = when {
            status >= PowerManager.THERMAL_STATUS_SEVERE -> TARGET_30_NS
            throttling -> TARGET_60_NS
            else -> PREFERRED_UPDATE_RATE_NS
        }
        preferEfficiency = throttling
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            applyThermalTarget(changed)
        }
    }

    /** API 33 entry. Not called on older releases, so [HintSession] stays unloaded there. */
    private fun ensureHintSession() {
        HintSession.ensure(appContext, targetWorkNs, preferEfficiency)
    }

    private fun closeHintSession() {
        HintSession.close()
    }

    private fun reportFrame(workDurationNs: Long) {
        HintSession.ensure(appContext, targetWorkNs, preferEfficiency)
        HintSession.report(workDurationNs)
    }

    private fun applyThermalTarget(logChange: Boolean) {
        HintSession.applyTarget(targetWorkNs, preferEfficiency)
        if (!logChange) {
            return
        }
        if (preferEfficiency) {
            Log.info("ADPF thermal backoff status $thermalStatus, target $targetWorkNs ns")
        } else {
            Log.info("ADPF thermal clear, target $PREFERRED_UPDATE_RATE_NS ns")
        }
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    private fun registerThermalListener() {
        if (thermalListener != null) {
            return
        }
        val context = appContext ?: return
        val powerManager = context.getSystemService(PowerManager::class.java) ?: return
        val listener = PowerManager.OnThermalStatusChangedListener { status ->
            onThermalStatus(status)
        }
        try {
            powerManager.addThermalStatusListener(context.mainExecutor, listener)
        } catch (e: RuntimeException) {
            Log.warning("ADPF thermal listener was not registered: ${e.message}")
            return
        }
        thermalListener = listener
        onThermalStatus(powerManager.currentThermalStatus)
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    private fun unregisterThermalListener() {
        val listener = thermalListener as? PowerManager.OnThermalStatusChangedListener ?: return
        thermalListener = null
        val context = appContext ?: return
        val powerManager = context.getSystemService(PowerManager::class.java) ?: return
        try {
            powerManager.removeThermalStatusListener(listener)
        } catch (e: RuntimeException) {
            Log.warning("ADPF thermal listener remove failed: ${e.message}")
        }
    }
}

@RequiresApi(Build.VERSION_CODES.TIRAMISU)
private object HintSession {
    private const val MIN_REPORT_NS = 1_000_000L
    private const val MAX_REPORT_NS = 100_000_000L

    private var session: PerformanceHintManager.Session? = null
    private var sessionTids: IntArray = IntArray(0)

    fun ensure(context: Context?, targetWorkNs: Long, preferEfficiency: Boolean) {
        val tids = readTids()
        if (tids.isEmpty()) {
            return
        }
        if (session != null && tids.contentEquals(sessionTids)) {
            return
        }
        open(context, tids, targetWorkNs, preferEfficiency)
    }

    fun report(workDurationNs: Long) {
        val current = session ?: return
        val reported = workDurationNs.coerceIn(MIN_REPORT_NS, MAX_REPORT_NS)
        try {
            current.reportActualWorkDuration(reported)
        } catch (e: RuntimeException) {
            Log.warning("ADPF reportActualWorkDuration failed: ${e.message}")
            close()
        }
    }

    fun applyTarget(targetWorkNs: Long, preferEfficiency: Boolean) {
        val current = session ?: return
        try {
            current.updateTargetWorkDuration(targetWorkNs)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
                current.setPreferPowerEfficiency(preferEfficiency)
            }
        } catch (e: RuntimeException) {
            Log.warning("ADPF thermal update failed: ${e.message}")
            close()
        }
    }

    fun close() {
        val current = session ?: return
        session = null
        sessionTids = IntArray(0)
        try {
            current.close()
        } catch (e: RuntimeException) {
            Log.warning("ADPF session close failed: ${e.message}")
        }
    }

    private fun open(
        context: Context?,
        tids: IntArray,
        targetWorkNs: Long,
        preferEfficiency: Boolean,
    ) {
        val appContext = context ?: return
        close()
        val manager = appContext.getSystemService(PerformanceHintManager::class.java)
        if (manager == null) {
            Log.info("ADPF PerformanceHintManager is not available")
            return
        }
        val created = try {
            manager.createHintSession(tids, targetWorkNs)
        } catch (e: RuntimeException) {
            Log.warning("ADPF createHintSession failed: ${e.message}")
            null
        }
        if (created == null) {
            Log.info("ADPF hint session was not created")
            return
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
            created.setPreferPowerEfficiency(preferEfficiency)
        }
        session = created
        sessionTids = tids.copyOf()
        Log.info(
            "ADPF hint session target $targetWorkNs ns " +
                "(preferred update ${AdpfBooster.PREFERRED_UPDATE_RATE_NS} ns, 8.33 ms, 120 fps), " +
                "device preferred rate ${manager.preferredUpdateRateNanos} ns, tids ${tids.size}"
        )
    }

    private fun readTids(): IntArray {
        return try {
            NativeLibrary.getEmulationThreadIds().filter { it > 0 }.distinct().toIntArray()
        } catch (e: UnsatisfiedLinkError) {
            IntArray(0)
        } catch (e: RuntimeException) {
            IntArray(0)
        }
    }
}
