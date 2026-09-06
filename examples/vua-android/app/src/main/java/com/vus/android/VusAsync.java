/*
 * VusAsync.java — 后台执行 + 主线程规避收敛（零依赖）
 *
 * Android 禁止主线程联网/重活；多处代码此前用 "new Thread + CountDownLatch +
 * 结果/异常格数组" 四联模板各写一遍。本类收敛为一个入口：
 *   VusAsync.bg(job, timeoutSec) —— 非主线程直接同步执行；主线程转子线程并阻塞等待。
 * 返回 Holder（val/err），调用方按需判失败/超时。
 *
 * 注：规避 d8 8.2 dev 对泛型匿名子类 desugar 的 NPE bug，不使用 lambda。
 */
package com.vus.android;

import android.os.Looper;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

final class VusAsync {

    /** 一次后台任务。抛出的任何异常记入 Holder.err。 */
    interface Job<T> {
        T run() throws Throwable;
    }

    /** 结果容器：val=正常结果；err=异常/超时（超时是 InterruptedException）。 */
    static final class Holder<T> {
        T val;
        Throwable err;
    }

    static boolean isMain() {
        return Looper.myLooper() == Looper.getMainLooper();
    }

    /**
     * 执行 job：非主线程直接同步跑；主线程转子线程阻塞等待（最多 timeoutSec 秒）。
     * 永不抛异常（全部折入 Holder.err）；job 本身未跑完的超时同样折入。
     */
    static <T> Holder<T> bg(final Job<T> job, final long timeoutSec) {
        final Holder<T> h = new Holder<>();
        Runnable run = new Runnable() {
            @Override public void run() {
                try { h.val = job.run(); }
                catch (Throwable t) { h.err = t; }
            }
        };
        if (!isMain()) {
            run.run();
            return h;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        new Thread(new Runnable() {
            @Override public void run() {
                try { run.run(); }
                finally { latch.countDown(); }
            }
        }).start();
        try {
            latch.await(timeoutSec, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            h.err = e;          // 主线程被意外打断视为失败
        }
        return h;
    }

    private VusAsync() { }
}