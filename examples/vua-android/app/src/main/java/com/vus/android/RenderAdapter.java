/*
 * RenderAdapter.java — 渲染适配器接口（M5 / vui · Gvui）
 *
 * vui 与宿主渲染实现之间的**唯一渲染树翻译通道**（设计 §3.4）：
 * 宿主（MainActivity 的版本号协议驱动、VusSession 的会话快照）只依赖本接口，
 * 不依赖具体渲染器。当前实现 = Gvui（VuaRenderer，Android View 树）；将来 vkt
 * （Kotlin）只是第二个实现——新的每个控件/每个 case 都搬到自己的适配器里，
 * 不再改动宿主流程。接口 4 个方法恰好覆盖宿主侧真实调用面：
 *
 *   - render(long)            版本号协议入口：指纹相同零工作；页面缓存命中直接
 *                             挂回；同屏增量 diff；否则全量重建（翻译通道主页）；
 *   - saveInputs()/savedVals() 会话快照要"输入控件当前值"（可序列化，红线 §6-6）；
 *   - restoreSaved(Map)        恢复快照状态，写回保存表、重建后按节点 restore 生效。
 *
 * 渲染器内部机制（页面 LRU / View diff / 子树复用 / Controls 分发表 / Theme）属
 * 各实现私有，不纳入接口——接口只描述"宿主如何驱动渲染器"，不描述"怎么渲染"。
 */
package com.vus.android;

import java.util.Map;

public interface RenderAdapter {

    /** 版本号协议渲染入口。本实现由 native 重绘回调经 MainActivity.renderCurrent
     *  调用；fp 为渲染树指纹（vuaRenderHash），命中缓存时连渲染树 JSON 都不取。 */
    void render(long fp);

    /** 重建前保存所有输入控件当前值，供重建后恢复（会话快照时借用）。
     *  由宿主在快照流程调用；实现内部在整树重建前也须自行调用（原子性）。 */
    void saveInputs();

    /** 当前输入控件值保存表（variable/id -> 值，可序列化）。快照时逐键读取，
     *  调用方无需知道具体控件形态。 */
    Map<String, String> savedVals();

    /** 恢复会话快照状态：把快照的变量/控件值写回保存表（随后按节点 restore 生效）。 */
    void restoreSaved(Map<String, String> vals);
}