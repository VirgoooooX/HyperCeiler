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

import android.telephony.SubscriptionManager
import android.view.View
import android.view.ViewGroup
import com.sevtinge.hyperceiler.common.log.XposedLog
import com.sevtinge.hyperceiler.libhook.appbase.systemui.StatusBarHook
import com.sevtinge.hyperceiler.libhook.utils.hookapi.systemui.MobileClass.miuiMobileIconBinder
import com.sevtinge.hyperceiler.libhook.utils.hookapi.systemui.MobileClass.modernStatusBarMobileView
import com.sevtinge.hyperceiler.libhook.utils.hookapi.systemui.MobileViewHelper
import io.github.lingqiqi5211.ezhooktool.core.callMethodAs
import io.github.lingqiqi5211.ezhooktool.core.findMethod
import io.github.lingqiqi5211.ezhooktool.xposed.dsl.getIntField
import io.github.lingqiqi5211.ezhooktool.xposed.dsl.createAfterHook
import java.util.concurrent.ConcurrentHashMap

/**
 * 移动信号 Hook 扩展基类
 *
 * 继承 [StatusBarHook]，封装 mobile 场景的通用 Hook 入口和工具。
 */
abstract class MobileSignalHook : StatusBarHook() {

    private val suppressedDualRowRoots = ConcurrentHashMap.newKeySet<Int>()
    private val suppressedDualRowRootWidths = ConcurrentHashMap<Int, Int>()

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
     * Hook ModernStatusBarMobileView.constructAndBind
     * @param callback 回调 (rootView, subId)
     */
    protected fun hookConstructAndBind(callback: (ViewGroup, Int) -> Unit) {
        modernStatusBarMobileView.findMethod { name("constructAndBind") }
            .createAfterHook { param ->
                val rootView = param.result as? ViewGroup ?: return@createAfterHook
                val subId = rootView.getIntField("subId")
                if (suppressDuplicateDualRowRoot(rootView, subId)) return@createAfterHook
                try {
                    callback(rootView, subId)
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
                if (subId >= 0 && suppressDuplicateDualRowRoot(container, subId)) {
                    return@createAfterHook
                }
                val binding = param.result ?: return@createAfterHook

                val tintFlow = findTintLightColorFlow(binding)
                if (tintFlow == null) {
                    XposedLog.w(TAG, lpparam.packageName, "hookDarkMode: tintLightColorFlow not found")
                    return@createAfterHook
                }

                MobileViewHelper.collectFlow(container, tintFlow) { triple ->
                    try {
                        // Re-assert host ownership on every tint emission. Modern
                        // pipeline collectors can change visibility after bind.
                        if (subId >= 0 && suppressDuplicateDualRowRoot(container, subId)) {
                            return@collectFlow
                        }
                        callback(container, extractDarkInfo(triple))
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
}
