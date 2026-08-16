/*
 * This file is part of HyperCeiler.
 *
 * HyperCeiler is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2023-2026 HyperCeiler Contributions
 */
package com.sevtinge.hyperceiler.libhook.rules.systemui.statusbar.mobile

import android.os.SystemClock
import android.telephony.SubscriptionManager
import android.view.View
import android.view.ViewGroup
import android.view.ViewTreeObserver
import com.sevtinge.hyperceiler.common.log.XposedLog
import com.sevtinge.hyperceiler.libhook.appbase.systemui.StatusBarHook
import com.sevtinge.hyperceiler.libhook.utils.hookapi.systemui.MobileClass.miuiMobileIconBinder
import com.sevtinge.hyperceiler.libhook.utils.hookapi.systemui.MobileClass.modernStatusBarMobileView
import com.sevtinge.hyperceiler.libhook.utils.hookapi.systemui.MobileViewHelper
import io.github.lingqiqi5211.ezhooktool.core.callMethodAs
import io.github.lingqiqi5211.ezhooktool.core.findMethod
import io.github.lingqiqi5211.ezhooktool.xposed.dsl.createAfterHook
import io.github.lingqiqi5211.ezhooktool.xposed.dsl.createInterceptHook
import io.github.lingqiqi5211.ezhooktool.xposed.dsl.getIntField
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 移动信号 Hook 扩展基类
 *
 * 继承 [StatusBarHook]，封装 mobile 场景的通用 Hook 入口和工具。
 */
abstract class MobileSignalHook : StatusBarHook() {

    private val suppressedDualRowRoots = ConcurrentHashMap.newKeySet<Int>()
    private val suppressedDualRowRootWidths = ConcurrentHashMap<Int, Int>()
    private val dualRowPreDrawRoots = ConcurrentHashMap.newKeySet<Int>()
    private val suppressedStockSignalViews = ConcurrentHashMap.newKeySet<Int>()
    private val dualContainerMarkerId by lazy { getOrCreateViewId("dual_signal_container") }

    // Temporary HyperOS 4 trace instrumentation.  It only records during the
    // first TRACE_WINDOW_MS after the hook is installed, and does not alter the
    // already verified dual-row ownership behavior.
    private val dualRowTraceInstalled = AtomicBoolean(false)
    private val dualRowTraceOwners = ConcurrentHashMap.newKeySet<Int>()
    private val dualRowTraceFlows = ConcurrentHashMap.newKeySet<Int>()
    private val dualRowTraceFlowValues = ConcurrentHashMap<Int, String>()

    @Volatile
    private var dualRowTraceUntilUptimeMs = 0L

    private fun installDualRowTrace() {
        if (this !is DualRowSignalHookV) return
        if (!dualRowTraceInstalled.compareAndSet(false, true)) return

        dualRowTraceUntilUptimeMs = SystemClock.uptimeMillis() + TRACE_WINDOW_MS
        XposedLog.i(
            TAG,
            lpparam.packageName,
            "DualRowTrace: armed visibility + mobile StateFlow trace window=${TRACE_WINDOW_MS}ms"
        )

        val setVisibility = View::class.java.getDeclaredMethod(
            "setVisibility",
            Int::class.javaPrimitiveType!!
        )
        setVisibility.createInterceptHook { chain ->
            val view = chain.thisObject as? View ?: return@createInterceptHook chain.proceed()
            val requested = chain.args.getOrNull(0) as? Int
                ?: return@createInterceptHook chain.proceed()

            if (!dualRowTraceActive() || !isDualRowTraceTarget(view)) {
                return@createInterceptHook chain.proceed()
            }

            val before = view.visibility
            // We specifically need the upstream writer that revives a stock/duplicate
            // mobile view.  HyperCeiler's own GONE writes are intentionally ignored.
            val traceRevive = requested == View.VISIBLE && before != View.VISIBLE
            val caller = if (traceRevive) dualRowTraceCallerStack() else ""
            val result = chain.proceed()

            if (traceRevive) {
                val after = view.visibility
                XposedLog.i(
                    TAG,
                    lpparam.packageName,
                    "DualRowTrace VIS subId=${findTraceSubId(view)} view=${traceViewName(view)} " +
                        "before=${visibilityName(before)} requested=VISIBLE after=${visibilityName(after)} " +
                        "class=${view.javaClass.name} caller=$caller"
                )
            }
            result
        }
    }

    private fun dualRowTraceActive(): Boolean =
        this is DualRowSignalHookV && SystemClock.uptimeMillis() <= dualRowTraceUntilUptimeMs

    private fun isDualRowTraceTarget(view: View): Boolean {
        if (view.javaClass.simpleName == "ModernStatusBarMobileView") return true
        return when (traceViewName(view)) {
            "mobile_signal", "mobile_signal_container", "mobile_group" -> true
            else -> false
        }
    }

    private fun traceViewName(view: View): String {
        if (view.id == View.NO_ID) return "<no-id>"
        return runCatching { view.resources.getResourceEntryName(view.id) }
            .getOrElse { "id=${view.id}" }
    }

    private fun findTraceSubId(view: View): Int {
        var current: View? = view
        repeat(10) {
            val candidate = current ?: return -1
            if (candidate.javaClass.simpleName == "ModernStatusBarMobileView") {
                return runCatching { candidate.getIntField("subId") }.getOrDefault(-1)
            }
            current = candidate.parent as? View
        }
        return -1
    }

    private fun visibilityName(value: Int): String = when (value) {
        View.VISIBLE -> "VISIBLE"
        View.INVISIBLE -> "INVISIBLE"
        View.GONE -> "GONE"
        else -> value.toString()
    }

    private fun dualRowTraceCallerStack(): String {
        val systemUiFrames = Thread.currentThread().stackTrace.asSequence()
            .filter { frame ->
                frame.className.startsWith("com.android.systemui") ||
                    frame.className.startsWith("kotlinx.coroutines")
            }
            .take(14)
            .map { frame -> "${frame.className}.${frame.methodName}:${frame.lineNumber}" }
            .toList()
        if (systemUiFrames.isNotEmpty()) return systemUiFrames.joinToString(" <- ")

        return Thread.currentThread().stackTrace.asSequence()
            .filterNot { frame ->
                frame.className.startsWith("java.lang.Thread") ||
                    frame.className.startsWith("io.github.lingqiqi5211") ||
                    frame.className.startsWith("io.github.libxposed")
            }
            .take(10)
            .joinToString(" <- ") { frame ->
                "${frame.className}.${frame.methodName}:${frame.lineNumber}"
            }
    }

    private fun traceMobileBinderObjects(
        container: ViewGroup,
        subId: Int,
        args: Array<out Any?>,
        binding: Any,
    ) {
        if (!dualRowTraceActive()) return

        args.forEachIndexed { index, value ->
            if (value != null) traceMobileOwner(container, subId, "bind.arg$index", value, 0)
        }
        traceMobileOwner(container, subId, "bind.result", binding, 0)
    }

    private fun traceMobileOwner(
        container: ViewGroup,
        subId: Int,
        label: String,
        owner: Any,
        depth: Int,
    ) {
        if (!dualRowTraceActive()) return
        if (owner is View || owner is String || owner is Number || owner is Boolean || owner.javaClass.isEnum) return

        val className = owner.javaClass.name
        val interestingOwner = className.startsWith("com.android.systemui") ||
            className.startsWith("kotlinx.coroutines")
        if (!interestingOwner) return

        val identity = System.identityHashCode(owner)
        if (!dualRowTraceOwners.add(identity)) return

        XposedLog.i(
            TAG,
            lpparam.packageName,
            "DualRowTrace OWNER subId=$subId label=$label class=$className"
        )

        var current: Class<*>? = owner.javaClass
        while (current != null && current != Any::class.java) {
            current.declaredFields.forEach { field ->
                val value = runCatching {
                    field.isAccessible = true
                    field.get(owner)
                }.getOrNull() ?: return@forEach

                val fieldPath = "$label.${field.name}"
                if (isStateFlowLike(value)) {
                    traceStateFlow(container, subId, owner.javaClass.name, fieldPath, value)
                } else if (depth == 0 && shouldTraceNestedOwner(value)) {
                    traceMobileOwner(container, subId, fieldPath, value, depth + 1)
                }
            }
            current = current.superclass
        }
    }

    private fun shouldTraceNestedOwner(value: Any): Boolean {
        val name = value.javaClass.name
        return name.startsWith("com.android.systemui.statusbar.pipeline.mobile") ||
            name.contains("MiuiCellularIconVM") ||
            name.contains("MobileIconViewModel")
    }

    private fun isStateFlowLike(value: Any): Boolean {
        val clazz = value.javaClass
        if (clazz.name.contains("StateFlow", ignoreCase = true)) return true
        return clazz.interfaces.any { it.name.contains("StateFlow", ignoreCase = true) }
    }

    private fun traceStateFlow(
        container: ViewGroup,
        subId: Int,
        ownerClass: String,
        fieldPath: String,
        flow: Any,
    ) {
        if (dualRowTraceFlows.size >= MAX_TRACED_FLOWS) return
        val identity = System.identityHashCode(flow)
        if (!dualRowTraceFlows.add(identity)) return

        val initial = readTraceFlowValue(flow)
        val initialSignature = traceValue(initial)
        dualRowTraceFlowValues[identity] = initialSignature
        XposedLog.i(
            TAG,
            lpparam.packageName,
            "DualRowTrace FLOW subId=$subId owner=$ownerClass field=$fieldPath " +
                "flow=${flow.javaClass.name} initial=$initialSignature"
        )

        MobileViewHelper.collectFlow(container, flow) { emitted ->
            if (!dualRowTraceActive()) return@collectFlow
            val signature = traceValue(emitted)
            if (dualRowTraceFlowValues.put(identity, signature) != signature) {
                XposedLog.i(
                    TAG,
                    lpparam.packageName,
                    "DualRowTrace EMIT subId=$subId field=$fieldPath value=$signature"
                )
            }
        }
    }

    private fun readTraceFlowValue(flow: Any): Any? = runCatching {
        val method = flow.javaClass.methods.firstOrNull {
            it.name == "getValue" && it.parameterCount == 0
        } ?: return@runCatching null
        method.isAccessible = true
        method.invoke(flow)
    }.getOrNull()

    private fun traceValue(value: Any?): String {
        if (value == null) return "null"
        val text = runCatching { value.toString() }.getOrDefault("<toString failed>")
        val clipped = if (text.length > 220) text.take(220) + "…" else text
        return "${value.javaClass.simpleName}($clipped)"
    }

    private fun hardCollapseDualRowRoot(rootView: ViewGroup, subId: Int, reason: String) {
        val identity = System.identityHashCode(rootView)
        val lp = rootView.layoutParams
        if (lp != null) {
            suppressedDualRowRootWidths.putIfAbsent(identity, lp.width)
            if (lp.width != 0) {
                lp.width = 0
                rootView.layoutParams = lp
            }
        }
        // HyperOS 4's binder/flows may restore visibility after constructAndBind.
        // Width=0 + alpha=0 keeps the duplicate physically absent even if a later
        // collector toggles visibility back to VISIBLE.
        rootView.alpha = 0f
        rootView.visibility = View.GONE
        if (suppressedDualRowRoots.add(identity)) {
            XposedLog.i(
                TAG,
                lpparam.packageName,
                "DualRowSignal: suppress duplicate mobile root subId=$subId $reason root=${rootView.javaClass.name}"
            )
        }
    }

    private fun restoreDualRowHostIfNeeded(rootView: ViewGroup) {
        val identity = System.identityHashCode(rootView)
        val oldWidth = suppressedDualRowRootWidths.remove(identity) ?: return
        val lp = rootView.layoutParams
        if (lp != null) {
            lp.width = oldWidth
            rootView.layoutParams = lp
        }
        rootView.alpha = 1f
        rootView.visibility = View.VISIBLE
        suppressedDualRowRoots.remove(identity)
    }

    /**
     * HyperOS 4 为每个订阅各创建一张 ModernStatusBarMobileView，而
     * DualRowSignalHookV 在一个容器内已经同时绘制 SIM1/SIM2。
     *
     * 优先把默认数据卡对应的 root 作为唯一宿主；这比启动早期调用
     * getSlotIndex() 更可靠，因为后者可能暂时返回 INVALID_SIM_SLOT_INDEX。
     * 若默认数据卡尚不可用，再回退到 slot 0。
     */
    protected fun suppressDuplicateDualRowRoot(rootView: ViewGroup, subId: Int): Boolean {
        if (this !is DualRowSignalHookV) return false

        val defaultDataSubId = SubscriptionManager.getDefaultDataSubscriptionId()
        val slot = SubscriptionManager.getSlotIndex(subId)
        val hasDefaultDataSub = defaultDataSubId >= 0 &&
            defaultDataSubId != SubscriptionManager.INVALID_SUBSCRIPTION_ID

        val suppress = if (hasDefaultDataSub) {
            subId != defaultDataSubId
        } else {
            slot > 0
        }

        if (suppress) {
            hardCollapseDualRowRoot(
                rootView,
                subId,
                "slot=$slot defaultDataSubId=$defaultDataSubId"
            )
            return true
        }

        restoreDualRowHostIfNeeded(rootView)
        return false
    }

    /**
     * The stock mobile_signal is owned by a separate HyperOS 4 collector.  It can
     * turn VISIBLE again after DualRowSignalHookV has rendered successfully,
     * producing exactly one custom dual-row icon plus one stock single-row icon.
     * Re-assert ownership immediately before every draw.  Alpha/scale are also
     * clamped so a later visibility-only update cannot make the stock glyph reappear.
     */
    private fun enforceDualRowVisualOwnership(rootView: ViewGroup, subId: Int): Boolean {
        if (this !is DualRowSignalHookV) return false
        if (suppressDuplicateDualRowRoot(rootView, subId)) return true

        val dualContainer = rootView.findViewById<View>(dualContainerMarkerId) ?: return false
        if (dualContainer.visibility != View.VISIBLE) return false

        val stockSignal = rootView.findById<View>("mobile_signal") ?: return false
        stockSignal.visibility = View.GONE
        stockSignal.alpha = 0f
        stockSignal.scaleX = 0f
        stockSignal.scaleY = 0f
        val identity = System.identityHashCode(stockSignal)
        if (suppressedStockSignalViews.add(identity)) {
            XposedLog.i(
                TAG,
                lpparam.packageName,
                "DualRowSignal: stock mobile_signal permanently suppressed on host subId=$subId root=${rootView.javaClass.name}"
            )
        }
        return false
    }

    private fun registerDualRowPreDrawGuard(rootView: ViewGroup, subId: Int) {
        if (this !is DualRowSignalHookV) return
        val identity = System.identityHashCode(rootView)
        if (!dualRowPreDrawRoots.add(identity)) return

        lateinit var preDrawListener: ViewTreeObserver.OnPreDrawListener
        val attachListener = object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: View) = Unit

            override fun onViewDetachedFromWindow(v: View) {
                if (rootView.viewTreeObserver.isAlive) {
                    rootView.viewTreeObserver.removeOnPreDrawListener(preDrawListener)
                }
                rootView.removeOnAttachStateChangeListener(this)
                dualRowPreDrawRoots.remove(identity)
            }
        }
        preDrawListener = ViewTreeObserver.OnPreDrawListener {
            enforceDualRowVisualOwnership(rootView, subId)
            true
        }
        rootView.addOnAttachStateChangeListener(attachListener)
        if (rootView.viewTreeObserver.isAlive) {
            rootView.viewTreeObserver.addOnPreDrawListener(preDrawListener)
        }
    }

    /**
     * Hook ModernStatusBarMobileView.constructAndBind
     * @param callback 回调 (rootView, subId)
     */
    protected fun hookConstructAndBind(callback: (ViewGroup, Int) -> Unit) {
        installDualRowTrace()
        modernStatusBarMobileView.findMethod { name("constructAndBind") }
            .createAfterHook { param ->
                val rootView = param.result as? ViewGroup ?: return@createAfterHook
                val subId = rootView.getIntField("subId")
                registerDualRowPreDrawGuard(rootView, subId)
                if (suppressDuplicateDualRowRoot(rootView, subId)) return@createAfterHook
                try {
                    callback(rootView, subId)
                    enforceDualRowVisualOwnership(rootView, subId)
                } catch (e: Throwable) {
                    XposedLog.e(TAG, lpparam.packageName, "hookConstructAndBind callback error", e)
                }
            }
    }

    /**
     * 信号反色逻辑
     *
     * @param callback 回调 (rootView, darkInfo)
     */
    protected fun hookDarkMode(callback: (ViewGroup, DarkInfo) -> Unit) {
        miuiMobileIconBinder.findMethod { name("bind") }
            .createAfterHook { param ->
                val container = param.args[0] as? ViewGroup ?: return@createAfterHook
                val subId = runCatching { container.getIntField("subId") }.getOrDefault(-1)
                val binding = param.result ?: return@createAfterHook

                // Trace before suppressing a duplicate root so we can see the exact
                // ViewModel/StateFlow graph that owns both host and secondary roots.
                traceMobileBinderObjects(container, subId, param.args, binding)

                if (subId >= 0) {
                    registerDualRowPreDrawGuard(container, subId)
                    if (enforceDualRowVisualOwnership(container, subId)) {
                        return@createAfterHook
                    }
                }

                val tintFlow = findTintLightColorFlow(binding)
                if (tintFlow == null) {
                    XposedLog.w(TAG, lpparam.packageName, "hookDarkMode: tintLightColorFlow not found")
                    return@createAfterHook
                }

                MobileViewHelper.collectFlow(container, tintFlow) { triple ->
                    try {
                        // Re-assert host ownership on every tint emission. Modern
                        // pipeline collectors can change visibility after bind.
                        if (subId >= 0 && enforceDualRowVisualOwnership(container, subId)) {
                            return@collectFlow
                        }
                        callback(container, extractDarkInfo(triple))
                        if (subId >= 0) enforceDualRowVisualOwnership(container, subId)
                    } catch (e: Throwable) {
                        XposedLog.e(TAG, lpparam.packageName, "hookDarkMode flow error", e)
                    }
                }
            }
    }

    /** 从 binding 对象中找到 tintLightColorFlow（值为 Triple 的 StateFlowImpl 字段） */
    private fun findTintLightColorFlow(binding: Any): Any? {
        for (field in binding.javaClass.declaredFields) {
            try {
                field.isAccessible = true
                val value = field.get(binding) ?: continue
                if (value.javaClass.simpleName == "StateFlowImpl") {
                    val currentValue = value.callMethodAs<Any>("getValue")
                    if (currentValue.javaClass.simpleName == "Triple") {
                        return value
                    }
                }
            } catch (_: Throwable) { continue }
        }
        return null
    }

    /** 从 Triple(isUseTint, isLight, color) 提取 DarkInfo */
    private fun extractDarkInfo(triple: Any): DarkInfo {
        val cls = triple.javaClass
        return DarkInfo.fromTintLightColor(
            isUseTint = cls.getMethod("getFirst").invoke(triple) as Boolean,
            isLight = cls.getMethod("getSecond").invoke(triple) as Boolean,
            color = cls.getMethod("getThird").invoke(triple) as Int
        )
    }

    /**
     * Hook MiuiMobileIconBinder.bind，在官方 bind 完成后回调。
     * 可用于修改已有控件属性/布局，或注入新组件。
     */
    protected fun hookBind(callback: (ViewGroup, Any) -> Unit) {
        miuiMobileIconBinder.findMethod { name("bind") }
            .createAfterHook { param ->
                val container = param.args[0] as? ViewGroup ?: return@createAfterHook
                val binding = param.result ?: return@createAfterHook
                try {
                    callback(container, binding)
                } catch (e: Throwable) {
                    XposedLog.e(TAG, lpparam.packageName, "hookBind callback error", e)
                }
            }
    }

    // ==================== 信号相关工具 ====================

    protected fun getDefaultDataSubId(): Int {
        return SubscriptionManager.getDefaultDataSubscriptionId()
    }

    protected fun forEachMobileView(subId: Int, callback: (View) -> Unit) {
        MobileViewHelper.forEachMobileView(subId, viewCache, callback)
    }

    companion object {
        private const val TRACE_WINDOW_MS = 90_000L
        private const val MAX_TRACED_FLOWS = 32
    }
}
