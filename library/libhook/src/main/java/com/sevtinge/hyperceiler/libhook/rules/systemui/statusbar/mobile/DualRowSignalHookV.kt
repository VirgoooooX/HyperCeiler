/*
 * This file is part of HyperCeiler.
 *
 * HyperCeiler is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 */
package com.sevtinge.hyperceiler.libhook.rules.systemui.statusbar.mobile

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.PorterDuff
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.telephony.SubscriptionManager
import android.util.SparseArray
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import androidx.core.graphics.createBitmap
import com.sevtinge.hyperceiler.common.log.XposedLog
import com.sevtinge.hyperceiler.common.utils.PrefsBridge
import com.sevtinge.hyperceiler.common.utils.api.ProjectApi
import com.sevtinge.hyperceiler.libhook.base.BaseHook
import com.sevtinge.hyperceiler.libhook.utils.api.DisplayUtils
import com.sevtinge.hyperceiler.libhook.utils.hookapi.systemui.MobileClass.mobileSignalController
import com.sevtinge.hyperceiler.libhook.utils.hookapi.systemui.MobileClass.networkController
import com.sevtinge.hyperceiler.libhook.utils.hookapi.tool.AppsTool.getModuleRes
import com.sevtinge.hyperceiler.libhook.utils.hookapi.tool.findViewByIdName
import com.sevtinge.hyperceiler.libhook.utils.hookapi.tool.getIdByName
import io.github.lingqiqi5211.ezhooktool.core.callMethodAs
import io.github.lingqiqi5211.ezhooktool.core.findMethod
import io.github.lingqiqi5211.ezhooktool.xposed.dsl.createInterceptHook
import io.github.lingqiqi5211.ezhooktool.xposed.dsl.getBooleanField
import io.github.lingqiqi5211.ezhooktool.xposed.dsl.getIntField
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

class DualRowSignalHookV : MobileSignalHook() {
    private val ID_DUAL_CONTAINER by lazy { getOrCreateViewId("dual_signal_container") }
    private val ID_SIGNAL_SLOT1 by lazy { getOrCreateViewId("dual_signal_slot1") }
    private val ID_SIGNAL_SLOT2 by lazy { getOrCreateViewId("dual_signal_slot2") }

    private val rightMargin by lazy {
        PrefsBridge.getInt("system_ui_statusbar_mobile_network_icon_right_margin", 8) - 8
    }
    private val leftMargin by lazy {
        PrefsBridge.getInt("system_ui_statusbar_mobile_network_icon_left_margin", 8) - 8
    }
    private val iconScale by lazy {
        PrefsBridge.getInt("system_ui_statusbar_mobile_network_icon_size", 100)
    }
    private val verticalOffset by lazy {
        PrefsBridge.getInt("system_ui_statusbar_mobile_network_icon_vertical_offset", 40)
    }
    private val selectedIconStyle by lazy {
        PrefsBridge.getString("system_ui_status_mobile_network_icon_style", "")
    }

    private val dualSignalResMap = HashMap<String, Bitmap>(64)
    private val simSignalLevels = ConcurrentHashMap<Int, Int>()
    private val simDataSimState = ConcurrentHashMap<Int, Boolean>()
    private val simSlotIndices = ConcurrentHashMap<Int, Int>()
    private val activeSubIds = ConcurrentHashMap.newKeySet<Int>()
    private val capturedSubIds = ConcurrentHashMap.newKeySet<Int>()
    private val missingViewWarnings = ConcurrentHashMap.newKeySet<String>()
    private val pendingViewLogs = ConcurrentHashMap.newKeySet<Int>()
    private val attachRefreshRegistered = ConcurrentHashMap.newKeySet<Int>()
    private val controllerFieldLogs = ConcurrentHashMap.newKeySet<String>()
    private val renderSignatures = ConcurrentHashMap<Int, String>()
    private val samplerRunning = AtomicBoolean(false)

    @Volatile
    private var networkControllerInstance: Any? = null

    @Volatile
    private var activeSubscriptionCount = -1

    @Volatile
    private var dualSignalResLoaded = false

    @Volatile
    private var samplerDeadlineUptimeMs = 0L

    private val viewDarkState = ConcurrentHashMap<Int, DarkInfo>()
    private val mainHandler by lazy { Handler(Looper.getMainLooper()) }

    override fun init() {
        BaseHook.registerHandlerHotReloadCleanup(mainHandler)
        listenMobileSignal()
        hookConstructAndBind { rootView, subId -> onViewCreated(rootView, subId) }
        hookDarkMode { rootView, darkInfo -> onDarkModeChanged(rootView, darkInfo) }
    }

    private fun onViewCreated(rootView: ViewGroup, subId: Int) {
        cacheTrackedView(subId, rootView)
        if (!ensureDualSignalContainer(rootView, subId)) return
        val dark = viewDarkState[System.identityHashCode(rootView)]
        refreshDualIconsForView(rootView, dark?.isUseTint ?: false, dark?.isLight ?: true, dark?.color)
    }

    private fun ensureDualSignalContainer(rootView: ViewGroup, subId: Int): Boolean {
        cacheTrackedView(subId, rootView)
        val activeCount = getActiveMobileControllerCount()
        if (activeCount <= 1) {
            syncDualSignalVisibility(rootView, false)
            val identity = System.identityHashCode(rootView)
            if (pendingViewLogs.add(identity)) {
                XposedLog.i(TAG, lpparam.packageName,
                    "DualRowSignal: cached mobile view subId=$subId while active subscriptions=$activeCount; waiting for telephony init")
            }
            return false
        }

        val mobileGroup = rootView.findById<View>("mobile_group")
        if (mobileGroup == null) {
            warnMissingViewOnce(rootView, "mobile_group")
            return false
        }
        val signalContainer = rootView.findById<ViewGroup>("mobile_signal_container")
        if (signalContainer == null) {
            warnMissingViewOnce(rootView, "mobile_signal_container")
            return false
        }
        if (rootView.findById<View>("mobile_signal") == null) {
            warnMissingViewOnce(rootView, "mobile_signal")
            return false
        }

        pendingViewLogs.remove(System.identityHashCode(rootView))
        ensureDualSignalResLoaded(rootView.context)
        if (!dualSignalResLoaded) {
            syncDualSignalVisibility(rootView, false)
            return false
        }

        mobileGroup.setPadding(
            DisplayUtils.dp2px(leftMargin * 0.5f), 0,
            DisplayUtils.dp2px(rightMargin * 0.5f), 0
        )

        val existing = rootView.findByViewId<FrameLayout>(ID_DUAL_CONTAINER)
        if (existing != null) return true

        val context = rootView.context
        val dualContainer = FrameLayout(context).apply {
            id = ID_DUAL_CONTAINER
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        }
        val slot1 = ImageView(context).apply { id = ID_SIGNAL_SLOT1; adjustViewBounds = true }
        val slot2 = ImageView(context).apply { id = ID_SIGNAL_SLOT2; adjustViewBounds = true }
        val signalHeight = if (iconScale != 100) {
            DisplayUtils.dp2px(iconScale / 10 * 2.0f)
        } else ViewGroup.LayoutParams.MATCH_PARENT
        val signalLp = ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, signalHeight)
        dualContainer.addView(slot1, ViewGroup.LayoutParams(signalLp))
        dualContainer.addView(slot2, ViewGroup.LayoutParams(signalLp))
        signalContainer.addView(dualContainer)

        val dualContainerId = ID_DUAL_CONTAINER
        runCatching {
            val dlp = dualContainer.layoutParams
            dlp.javaClass.getField("endToEnd").setInt(dlp, 0)
            dlp.javaClass.getField("topToTop").setInt(dlp, 0)
            dlp.javaClass.getField("bottomToBottom").setInt(dlp, 0)
            dualContainer.layoutParams = dlp
        }
        (signalContainer.findViewByIdName("mobile_type") as? ImageView)?.let { mobileType ->
            runCatching {
                val lp = mobileType.layoutParams
                lp.javaClass.getField("endToStart").setInt(lp, dualContainerId)
                lp.javaClass.getField("topToTop").setInt(lp, dualContainerId)
                mobileType.layoutParams = lp
            }
        }
        (signalContainer.findViewByIdName("mobile_left_mobile_inout") as? ImageView)?.let { inout ->
            runCatching {
                val lp = inout.layoutParams
                lp.javaClass.getField("endToStart").setInt(lp, dualContainerId)
                lp.javaClass.getField("bottomToBottom").setInt(lp, dualContainerId)
                lp.javaClass.getField("topToTop").setInt(lp, -1)
                inout.layoutParams = lp
            }
        }
        if (verticalOffset != 40) {
            dualContainer.translationY = DisplayUtils.dp2px((verticalOffset - 40) * 0.1f).toFloat()
        }

        syncDualSignalVisibility(rootView, false)
        XposedLog.i(TAG, lpparam.packageName,
            "DualRowSignal: dual container created for subId=$subId activeSubscriptions=$activeCount root=${rootView.javaClass.name} resources=${dualSignalResMap.size}")

        setDensityReplacement("com.android.systemui", "dimen", "status_bar_mobile_type_half_to_top_distance", 3f)
        setDensityReplacement("com.android.systemui", "dimen", "status_bar_mobile_left_inout_over_strength", 0f)
        setDensityReplacement("com.android.systemui", "dimen", "status_bar_mobile_type_middle_to_strength_start", -0.4f)
        return true
    }

    private fun cacheTrackedView(subId: Int, rootView: ViewGroup) {
        cacheView(subId, rootView)
        if (rootView.isAttachedToWindow) return
        val identity = System.identityHashCode(rootView)
        if (!attachRefreshRegistered.add(identity)) return
        rootView.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: View) {
                rootView.removeOnAttachStateChangeListener(this)
                attachRefreshRegistered.remove(identity)
                refreshAllCachedViews()
            }
            override fun onViewDetachedFromWindow(v: View) = Unit
        })
    }

    private fun warnMissingViewOnce(rootView: ViewGroup, idName: String) {
        val key = "${System.identityHashCode(rootView)}:$idName"
        if (!missingViewWarnings.add(key)) return
        XposedLog.w(TAG, lpparam.packageName,
            "DualRowSignal OS4 compatibility: required view '$idName' not found under ${rootView.javaClass.name}")
    }

    private fun onDarkModeChanged(rootView: ViewGroup, darkInfo: DarkInfo) {
        val subId = runCatching { getIntField(rootView, "subId") }.getOrDefault(-1)
        if (subId == -1) return
        viewDarkState[System.identityHashCode(rootView)] = darkInfo
        cacheTrackedView(subId, rootView)
        if (!ensureDualSignalContainer(rootView, subId)) return
        refreshDualIconsForView(rootView, darkInfo.isUseTint, darkInfo.isLight, darkInfo.color)
    }

    private fun getActiveMobileControllerCount(): Int {
        val authoritativeCount = activeSubscriptionCount
        if (authoritativeCount >= 0) return authoritativeCount
        val observedSlotCount = simSlotIndices.values
            .filter { it != SubscriptionManager.INVALID_SIM_SLOT_INDEX && it >= 0 }
            .toSet().size
        if (observedSlotCount > 0) return observedSlotCount
        return collectMobileControllers().mapNotNull { it.first }.toSet().size
    }

    private fun drawableToBitmap(drawable: Drawable): Bitmap {
        if (drawable is BitmapDrawable && drawable.bitmap != null) return drawable.bitmap
        val width = drawable.intrinsicWidth.takeIf { it > 0 } ?: 1
        val height = drawable.intrinsicHeight.takeIf { it > 0 } ?: 1
        val bitmap = createBitmap(width, height)
        val canvas = Canvas(bitmap)
        drawable.setBounds(0, 0, width, height)
        drawable.draw(canvas)
        return bitmap
    }

    private fun ensureDualSignalResLoaded(context: Context) {
        if (dualSignalResLoaded) return
        synchronized(this) {
            if (dualSignalResLoaded) return
            val modRes = getModuleRes(context.applicationContext ?: context)
            dualSignalResMap.clear()
            val colorModes = if (selectedIconStyle == "theme") {
                arrayOf(Triple("", false, true), Triple("dark", false, false))
            } else {
                arrayOf(Triple("", false, true), Triple("dark", false, false), Triple("tint", true, true))
            }
            for (slot in 1..2) {
                for (lvl in 0..5) {
                    for ((_, isUseTint, isLight) in colorModes) {
                        val resName = getSignalIconResName(slot, lvl, isUseTint, isLight)
                        val resId = modRes.getIdByName(resName, "drawable", ProjectApi.mAppModulePkg)
                        if (resId != 0) {
                            modRes.getDrawable(resId, null)?.let { drawable ->
                                dualSignalResMap[resName] = drawableToBitmap(drawable)
                            }
                        }
                    }
                }
            }
            dualSignalResLoaded = dualSignalResMap.isNotEmpty()
            XposedLog.i(TAG, lpparam.packageName,
                "DualRowSignal: resources loaded ok=$dualSignalResLoaded count=${dualSignalResMap.size} style='$selectedIconStyle'")
        }
    }

    private fun listenMobileSignal() {
        mobileSignalController.findMethod { name("notifyListeners") }
            .createInterceptHook { chain ->
                val result = chain.proceed()
                val signalController = chain.thisObject ?: return@createInterceptHook result
                if (networkControllerInstance == null) {
                    readField(signalController, "mNetworkController")?.let { network ->
                        networkControllerInstance = network
                        if (!controllerBootstrapReady()) startControllerSampler("network-controller")
                    }
                }
                if (captureSignalController(signalController, "notifyListeners", null)) refreshAllCachedViews()
                result
            }

        networkController.findMethod { name("setCurrentSubscriptionsLocked") }
            .createInterceptHook { chain ->
                val result = chain.proceed()
                val networkCtrl = chain.thisObject ?: return@createInterceptHook result
                networkControllerInstance = networkCtrl
                val subList = chain.args.getOrNull(0) as? List<*> ?: emptyList<Any>()
                val newSubIds = LinkedHashSet<Int>()
                val newSlotIndices = LinkedHashMap<Int, Int>()
                subList.filterNotNull().forEach { subInfo ->
                    val subId = runCatching { subInfo.callMethodAs<Int>("getSubscriptionId") }
                        .getOrNull() ?: return@forEach
                    newSubIds += subId
                    val slot = runCatching { subInfo.callMethodAs<Int>("getSimSlotIndex") }
                        .getOrElse { SubscriptionManager.getSlotIndex(subId) }
                    if (slot != SubscriptionManager.INVALID_SIM_SLOT_INDEX && slot >= 0) newSlotIndices[subId] = slot
                }
                activeSubIds.clear()
                activeSubIds.addAll(newSubIds)
                capturedSubIds.clear()
                val slotCount = newSlotIndices.values.toSet().size
                activeSubscriptionCount = if (slotCount > 0) slotCount else newSubIds.size
                if (newSubIds.isEmpty()) {
                    simSignalLevels.clear(); simDataSimState.clear(); simSlotIndices.clear()
                } else {
                    simSignalLevels.keys.filter { it !in newSubIds }.forEach {
                        simSignalLevels.remove(it); simDataSimState.remove(it); simSlotIndices.remove(it)
                    }
                    newSubIds.forEach { subId ->
                        simSignalLevels.putIfAbsent(subId, 0)
                        simDataSimState.putIfAbsent(subId, subId == SubscriptionManager.getDefaultDataSubscriptionId())
                        newSlotIndices[subId]?.let { simSlotIndices[subId] = it }
                    }
                }
                XposedLog.i(TAG, lpparam.packageName,
                    "DualRowSignal: subscriptions updated count=$activeSubscriptionCount ids=$newSubIds slots=$newSlotIndices")

                // Subscription topology changes always deserve one immediate UI pass.
                // Afterwards only actual controller-state changes trigger redraws.
                pollControllersOnce("subscriptions")
                refreshAllCachedViews()
                if (!controllerBootstrapReady()) startControllerSampler("subscriptions")
                result
            }
    }

    private fun startControllerSampler(reason: String) {
        val deadline = SystemClock.uptimeMillis() + BOOTSTRAP_WINDOW_MS
        if (deadline > samplerDeadlineUptimeMs) samplerDeadlineUptimeMs = deadline
        if (!samplerRunning.compareAndSet(false, true)) return
        XposedLog.i(TAG, lpparam.packageName,
            "DualRowSignal: OS4 controller bootstrap started reason=$reason window=${BOOTSTRAP_WINDOW_MS}ms")
        mainHandler.post(controllerPollRunnable)
    }

    private val controllerPollRunnable = object : Runnable {
        override fun run() {
            val changed = pollControllersOnce("bootstrap")
            if (changed) refreshAllCachedViews()

            val ready = controllerBootstrapReady()
            val timedOut = SystemClock.uptimeMillis() >= samplerDeadlineUptimeMs
            if (ready || timedOut) {
                samplerRunning.set(false)
                XposedLog.i(TAG, lpparam.packageName,
                    "DualRowSignal: OS4 controller bootstrap stopped ready=$ready captured=${capturedSubIds.size}/${activeSubIds.size} timedOut=$timedOut")
                return
            }
            mainHandler.postDelayed(this, BOOTSTRAP_POLL_INTERVAL_MS)
        }
    }

    private fun controllerBootstrapReady(): Boolean {
        val expected = activeSubIds.toList()
        if (expected.isEmpty()) return false
        return expected.all { subId ->
            capturedSubIds.contains(subId) &&
                (simSlotIndices[subId] ?: SubscriptionManager.INVALID_SIM_SLOT_INDEX) >= 0
        }
    }

    private fun pollControllersOnce(source: String): Boolean {
        var changed = false
        collectMobileControllers().forEach { (hintedSubId, controller) ->
            changed = captureSignalController(controller, source, hintedSubId) || changed
        }
        return changed
    }

    private fun collectMobileControllers(): List<Pair<Int?, Any>> {
        val network = networkControllerInstance ?: return emptyList()
        val result = ArrayList<Pair<Int?, Any>>(4)
        val seen = HashSet<Int>()
        fun appendCandidate(fieldName: String, value: Any?) {
            when (value) {
                is SparseArray<*> -> {
                    for (i in 0 until value.size()) {
                        val item = value.valueAt(i) ?: continue
                        if (!looksLikeSignalController(item)) continue
                        if (!seen.add(System.identityHashCode(item))) continue
                        result += value.keyAt(i) to item
                    }
                    if (result.isNotEmpty() && controllerFieldLogs.add(fieldName)) {
                        XposedLog.i(TAG, lpparam.packageName,
                            "DualRowSignal: controller source field=$fieldName type=SparseArray size=${value.size()}")
                    }
                }
                is Map<*, *> -> {
                    value.forEach { (key, item) ->
                        item ?: return@forEach
                        if (!looksLikeSignalController(item)) return@forEach
                        if (!seen.add(System.identityHashCode(item))) return@forEach
                        result += (key as? Int) to item
                    }
                    if (result.isNotEmpty() && controllerFieldLogs.add(fieldName)) {
                        XposedLog.i(TAG, lpparam.packageName,
                            "DualRowSignal: controller source field=$fieldName type=Map size=${value.size}")
                    }
                }
            }
        }
        appendCandidate("mMobileSignalControllers", readField(network, "mMobileSignalControllers"))
        if (result.isNotEmpty()) return result
        forEachField(network) { fieldName, value -> appendCandidate(fieldName, value) }
        if (result.isEmpty() && controllerFieldLogs.add("<missing>")) {
            XposedLog.w(TAG, lpparam.packageName,
                "DualRowSignal: no MobileSignalController collection found on ${network.javaClass.name}; keeping stock signal until controller data is available")
        }
        return result
    }

    private fun looksLikeSignalController(instance: Any): Boolean {
        if (instance.javaClass.name.contains("MobileSignalController")) return true
        return findCompatField(instance.javaClass, "mCurrentState") != null
    }

    private fun captureSignalController(signalController: Any, source: String, hintedSubId: Int?): Boolean {
        val subscriptionInfo = readField(signalController, "mSubscriptionInfo")
        val subscriptionId = runCatching { subscriptionInfo?.callMethodAs<Int>("getSubscriptionId") }
            .getOrNull() ?: hintedSubId ?: return false
        val slotIndex = runCatching { subscriptionInfo?.callMethodAs<Int>("getSimSlotIndex") }
            .getOrNull()?.takeIf { it != SubscriptionManager.INVALID_SIM_SLOT_INDEX && it >= 0 }
            ?: simSlotIndices[subscriptionId] ?: SubscriptionManager.getSlotIndex(subscriptionId)
        val currentState = readField(signalController, "mCurrentState")
            ?: readField(signalController, "mLastState") ?: return false
        val dataSim = runCatching { currentState.getBooleanField("dataSim") }
            .getOrElse { subscriptionId == SubscriptionManager.getDefaultDataSubscriptionId() }
        val enabled = runCatching { currentState.getBooleanField("enabled") }.getOrDefault(true)
        val connected = runCatching { currentState.getBooleanField("connected") }.getOrNull()
        val signalStrength = readField(currentState, "signalStrength")
        val miuiLevel = runCatching { signalStrength?.callMethodAs<Int>("getMiuiLevel") }.getOrNull()
        val androidLevel = runCatching { signalStrength?.callMethodAs<Int>("getLevel") }.getOrNull()
        val stateLevel = runCatching { currentState.getIntField("level") }.getOrNull()
        val alternateLevel = runCatching { currentState.getIntField("signalLevel") }.getOrNull()
        val rawLevel = listOfNotNull(miuiLevel, stateLevel, androidLevel, alternateLevel)
            .firstOrNull { it >= 0 } ?: 0
        val mappedLevel = (if (rawLevel >= 2) rawLevel + 1 else rawLevel).coerceIn(0, 5)
        val previousLevel = simSignalLevels[subscriptionId] ?: 0
        val level = if (connected == false && enabled && mappedLevel == 0 && previousLevel > 0) previousLevel else mappedLevel
        val oldLevel = simSignalLevels.put(subscriptionId, level)
        val oldDataSim = simDataSimState.put(subscriptionId, dataSim)
        val oldSlotIndex = simSlotIndices.put(subscriptionId, slotIndex)
        capturedSubIds.add(subscriptionId)
        val changed = oldLevel != level || oldDataSim != dataSim || oldSlotIndex != slotIndex
        if (changed) {
            XposedLog.i(TAG, lpparam.packageName,
                "DualRowSignal: signal captured subId=$subscriptionId slot=$slotIndex raw=$rawLevel level=$level connected=$connected dataSim=$dataSim source=$source controller=${signalController.javaClass.name}")
        }
        return changed
    }

    private fun findCompatField(clazz: Class<*>, name: String): java.lang.reflect.Field? {
        var current: Class<*>? = clazz
        while (current != null) {
            val cls = current
            val field = runCatching { cls.getDeclaredField(name) }.getOrNull()
            if (field != null) return field
            current = cls.superclass
        }
        return null
    }

    private fun readField(instance: Any, name: String): Any? {
        val field = findCompatField(instance.javaClass, name) ?: return null
        return runCatching { field.isAccessible = true; field.get(instance) }.getOrNull()
    }

    private inline fun forEachField(instance: Any, block: (String, Any?) -> Unit) {
        var current: Class<*>? = instance.javaClass
        while (current != null) {
            val cls = current
            cls.declaredFields.forEach { field ->
                val value = runCatching { field.isAccessible = true; field.get(instance) }.getOrNull()
                block(field.name, value)
            }
            current = cls.superclass
        }
    }

    private fun getSignalLevelsForRender(out: IntArray) {
        out[0] = 0; out[1] = 0
        val candidates = if (activeSubIds.isNotEmpty()) activeSubIds.toList() else simSignalLevels.keys.toList()
        val orderedSubIds = candidates.sortedWith(compareBy(
            { when { simDataSimState[it] == true -> 0; it == SubscriptionManager.getDefaultDataSubscriptionId() -> 1; else -> 2 } },
            { simSlotIndices[it] ?: Int.MAX_VALUE }, { it }
        ))
        orderedSubIds.getOrNull(0)?.let { out[0] = simSignalLevels[it] ?: 0 }
        orderedSubIds.getOrNull(1)?.let { out[1] = simSignalLevels[it] ?: 0 }
    }

    private val renderLevels = IntArray(2)

    private fun resolveSignalBitmap(slot: Int, level: Int, isUseTint: Boolean, isLight: Boolean): Pair<String, Bitmap>? {
        val requested = getSignalIconResName(slot, level, isUseTint, isLight)
        dualSignalResMap[requested]?.let { return requested to it }
        val base = getSignalIconResName(slot, level, false, true)
        dualSignalResMap[base]?.let { return base to it }
        return null
    }

    private fun refreshDualIconsForView(rootView: ViewGroup, isUseTint: Boolean, isLight: Boolean, color: Int? = null) {
        if (getActiveMobileControllerCount() <= 1 || !dualSignalResLoaded) {
            syncDualSignalVisibility(rootView, false); return
        }
        val dualContainer = rootView.findByViewId<FrameLayout>(ID_DUAL_CONTAINER)
        if (dualContainer == null) { syncDualSignalVisibility(rootView, false); return }
        val slot1 = dualContainer.findByViewId<ImageView>(ID_SIGNAL_SLOT1)
        val slot2 = dualContainer.findByViewId<ImageView>(ID_SIGNAL_SLOT2)
        if (slot1 == null || slot2 == null) { syncDualSignalVisibility(rootView, false); return }
        getSignalLevelsForRender(renderLevels)
        val dataLevel = renderLevels[0]
        val noDataLevel = renderLevels[1]
        val slot1Resolved = resolveSignalBitmap(1, dataLevel, isUseTint, isLight)
        val slot2Resolved = resolveSignalBitmap(2, noDataLevel, isUseTint, isLight)
        if (slot1Resolved == null || slot2Resolved == null) {
            syncDualSignalVisibility(rootView, false)
            XposedLog.w(TAG, lpparam.packageName,
                "DualRowSignal: render bitmap missing levels=[$dataLevel,$noDataLevel] tint=$isUseTint light=$isLight")
            return
        }

        val needsTint = isUseTint && selectedIconStyle != "theme"
        val tintSignature = if (needsTint) color else null
        val identity = System.identityHashCode(rootView)
        val signature = "$dataLevel/$noDataLevel:$isUseTint:$isLight:$tintSignature:${slot1Resolved.first}:${slot2Resolved.first}"
        if (renderSignatures[identity] == signature) {
            // Keep visibility ownership correct, but avoid bitmap, tint, layout and
            // invalidation work when the rendered state is byte-for-byte unchanged.
            syncDualSignalVisibility(rootView, true)
            return
        }

        slot1.setImageBitmap(slot1Resolved.second)
        slot2.setImageBitmap(slot2Resolved.second)
        if (needsTint && color != null) {
            slot1.setColorFilter(color, PorterDuff.Mode.SRC_IN)
            slot2.setColorFilter(color, PorterDuff.Mode.SRC_IN)
        } else {
            slot1.clearColorFilter(); slot2.clearColorFilter()
        }
        dualContainer.requestLayout()
        dualContainer.invalidate()
        syncDualSignalVisibility(rootView, true)
        renderSignatures[identity] = signature
        XposedLog.i(TAG, lpparam.packageName,
            "DualRowSignal: render levels=[$dataLevel,$noDataLevel] slot1=${slot1Resolved.first} slot2=${slot2Resolved.first} root=${rootView.javaClass.name}")
    }

    private val refreshRunnable = Runnable {
        viewCache.entries.forEach { (subId, viewSet) ->
            val snapshot = synchronized(viewSet) { viewSet.toList() }
            snapshot.forEach { rootView ->
                if (!rootView.isAttachedToWindow) return@forEach
                if (!ensureDualSignalContainer(rootView, subId)) return@forEach
                val dark = viewDarkState[System.identityHashCode(rootView)]
                refreshDualIconsForView(rootView, dark?.isUseTint ?: false, dark?.isLight ?: true, dark?.color)
            }
        }
    }

    private fun refreshAllCachedViews() {
        mainHandler.removeCallbacks(refreshRunnable)
        mainHandler.post(refreshRunnable)
    }

    private fun syncDualSignalVisibility(rootView: ViewGroup, useDualSignal: Boolean) {
        rootView.findByViewId<FrameLayout>(ID_DUAL_CONTAINER)?.visibility = if (useDualSignal) View.VISIBLE else View.GONE
        rootView.findById<View>("mobile_signal")?.visibility = if (useDualSignal) View.GONE else View.VISIBLE
    }

    private fun getSignalIconResName(slot: Int, level: Int, isUseTint: Boolean, isLight: Boolean): String {
        val iconStyle = if (selectedIconStyle.isNotEmpty()) "_$selectedIconStyle" else ""
        val colorMode = if (!isUseTint || selectedIconStyle == "theme") {
            if (!isLight) "_dark" else ""
        } else "_tint"
        return "statusbar_signal_${slot}_${level.coerceIn(0, 5)}$colorMode$iconStyle"
    }

    companion object {
        private const val BOOTSTRAP_POLL_INTERVAL_MS = 750L
        private const val BOOTSTRAP_WINDOW_MS = 6000L
    }
}
