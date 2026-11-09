import { For, Show, createSignal, onCleanup, onMount } from "solid-js";
import {
  getAccounts,
  getKlineCacheStatus,
  getReadyz,
  getTickCacheStatus,
  type AccountsResponse,
  type ReadyState,
  type TickCacheStatus,
} from "../api/client";
import { formatMoney } from "../lib/format";

export default function OpsPage() {
  const [ready, setReady] = createSignal<ReadyState>();
  const [accounts, setAccounts] = createSignal<AccountsResponse>();
  const [tickCache, setTickCache] = createSignal<TickCacheStatus>();
  const [klineCache, setKlineCache] = createSignal<Record<string, unknown>>();
  const [error, setError] = createSignal("");
  const [updatedAt, setUpdatedAt] = createSignal("");

  async function refresh() {
    const results = await Promise.allSettled([
      getReadyz(),
      getAccounts(),
      getTickCacheStatus(),
      getKlineCacheStatus(),
    ]);
    if (results[0].status === "fulfilled") setReady(results[0].value);
    if (results[1].status === "fulfilled") setAccounts(results[1].value);
    if (results[2].status === "fulfilled") setTickCache(results[2].value);
    if (results[3].status === "fulfilled") setKlineCache(results[3].value);
    const failed = results.find((item) => item.status === "rejected");
    setError(failed && failed.status === "rejected" ? String(failed.reason) : "");
    setUpdatedAt(new Date().toLocaleTimeString("zh-CN", { hour12: false }));
  }

  onMount(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 3000);
    onCleanup(() => window.clearInterval(timer));
  });

  return (
    <main class="ops-page">
      <div class="ops-heading">
        <div>
          <h2>运行状态</h2>
          <p class="muted">TRADING / READY / STORAGE · 每 3 秒刷新</p>
        </div>
        <div class="ops-heading-actions">
          <span class="muted mono">更新 {updatedAt() || "-"}</span>
          <button type="button" class="ghost-button" onClick={() => void refresh()}>
            刷新
          </button>
        </div>
      </div>

      <Show when={error()}>
        <div class="ops-error">{error()}</div>
      </Show>

      <section class="ops-grid">
        <div class="ops-card">
          <small>网关</small>
          <strong class={ready()?.ready ? "positive" : "warning-text"}>
            {ready()?.ready ? "READY" : "NOT READY"}
          </strong>
          <span>MD {ready()?.md?.connected ? "connected" : "offline"}</span>
          <span>TD {ready()?.td?.phase ?? "unknown"}</span>
          <span>expected disconnect: {String(Boolean(ready()?.td?.expected_disconnect))}</span>
        </div>

        <div class="ops-card">
          <small>Tick 缓存</small>
          <strong class={tickCache()?.ready ? "positive" : "warning-text"}>
            {tickCache()?.enabled ? (tickCache()?.ready ? "ONLINE" : "DEGRADED") : "DISABLED"}
          </strong>
          <span>accepted {tickCache()?.accepted ?? 0}</span>
          <span>written {tickCache()?.written ?? 0}</span>
          <span>pending {tickCache()?.pending ?? 0} · dropped {tickCache()?.dropped ?? 0}</span>
          <span class="mono ops-path">{tickCache()?.file ?? "-"}</span>
        </div>

        <div class="ops-card">
          <small>K 线缓存</small>
          <strong class={klineCache()?.ready ? "positive" : "warning-text"}>
            {klineCache()?.ready ? "ONLINE" : "DISABLED"}
          </strong>
          <span>written {String(klineCache()?.written ?? 0)}</span>
          <span>pending {String(klineCache()?.pending ?? 0)}</span>
          <span class="mono ops-path">{String(klineCache()?.file ?? "-")}</span>
        </div>
      </section>

      <section class="ops-section">
        <h3>账户</h3>
        <div class="ops-account-list">
          <For each={accounts()?.accounts ?? []}>
            {(item) => (
              <div class={`ops-account ${item.account_id === accounts()?.default ? "default" : ""}`}>
                <div class="ops-account-title">
                  <strong>{item.account_id}</strong>
                  <span class={item.ready ? "positive" : "warning-text"}>
                    {item.ready ? "READY" : "NOT READY"}
                  </span>
                  <Show when={item.account_id === accounts()?.default}>
                    <span class="source-badge live">DEFAULT</span>
                  </Show>
                </div>
                <div class="ops-account-metrics">
                  <span>权益 {formatMoney(item.account?.balance)}</span>
                  <span>可用 {formatMoney(item.account?.available)}</span>
                  <span>保证金 {formatMoney(item.account?.margin)}</span>
                  <span>风险度 {formatMoney(item.account?.risk_ratio)}</span>
                </div>
              </div>
            )}
          </For>
        </div>
      </section>

      <section class="ops-section">
        <h3>端点</h3>
        <div class="ops-link-grid">
          <code>GET /healthz</code>
          <code>GET /readyz</code>
          <code>GET /accounts</code>
          <code>GET /trading-status</code>
          <code>GET /tick-cache/status</code>
          <code>GET /kline-cache/status</code>
        </div>
      </section>
    </main>
  );
}
