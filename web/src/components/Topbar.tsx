import { For, Show } from "solid-js";
import type { AccountSnapshot, AccountsResponse, ReadyState } from "../api/client";
import type { Theme, View } from "../lib/storage";
import { formatMoney } from "../lib/format";

interface Props {
  view: View;
  theme: Theme;
  streamStatus: "connecting" | "online" | "offline";
  ready?: ReadyState;
  account?: AccountSnapshot;
  accounts?: AccountsResponse;
  selectedAccount: string;
  onAccount: (account: string) => void;
  onView: (view: View) => void;
  onTheme: () => void;
}

const NAV: Array<{ id: View; label: string }> = [
  { id: "terminal", label: "交易终端" },
  { id: "futures", label: "期货列表" },
  { id: "options", label: "期权链" },
  { id: "ops", label: "运维" },
];

export default function Topbar(props: Props) {
  const phase = () => props.ready?.td?.phase ?? "unknown";
  const online = () => props.streamStatus === "online";
  const tradingReady = () => Boolean(props.ready?.ready) && online();

  return (
    <header class="topbar">
      <div class="brand">
        <span class="brand-mark">TQ</span>
        <div>
          <strong>TQSDK Board</strong>
          <small>Futures workstation · REST + WebSocket</small>
        </div>
      </div>

      <nav class="main-nav" aria-label="主导航">
        <For each={NAV}>
          {(item) => (
            <button
              type="button"
              class={props.view === item.id ? "active" : ""}
              onClick={() => props.onView(item.id)}
            >
              {item.label}
            </button>
          )}
        </For>
      </nav>

      <div class="topbar-meta">
        <span class={`status-dot ${online() ? "live" : "offline"}`} />
        <span>
          {online() ? "行情在线" : props.streamStatus === "connecting" ? "连接中" : "已断开"}
        </span>
        <span class="divider" />
        <span>TD {phase()}</span>
        <Show when={props.ready?.td?.expected_disconnect}>
          <span class="warning-pill">结算预期断线</span>
        </Show>
        <span class="divider" />
        <span class={`source-badge ${tradingReady() ? "live" : ""}`}>
          {tradingReady() ? "LIVE" : "READONLY"}
        </span>
      </div>

      <label class="account-select">
        <small>账户</small>
        <select
          value={props.selectedAccount}
          onChange={(event) => props.onAccount(event.currentTarget.value)}
          aria-label="选择交易账户"
        >
          <For each={props.accounts?.accounts ?? []}>
            {(item) => <option value={item.account_id}>{item.account_id}</option>}
          </For>
        </select>
      </label>

      <div class="account-strip">
        <div>
          <small>权益</small>
          <strong>{formatMoney(props.account?.balance)}</strong>
        </div>
        <div>
          <small>可用</small>
          <strong>{formatMoney(props.account?.available)}</strong>
        </div>
        <div>
          <small>保证金</small>
          <strong>{formatMoney(props.account?.margin)}</strong>
        </div>
        <div class={(props.account?.float_profit ?? 0) >= 0 ? "positive" : "negative"}>
          <small>浮动盈亏</small>
          <strong>{formatMoney(props.account?.float_profit)}</strong>
        </div>
      </div>

      <button
        type="button"
        class="theme-toggle"
        onClick={props.onTheme}
        title="切换明暗主题"
      >
        {props.theme === "dark" ? "浅色" : "深色"}
      </button>
    </header>
  );
}
