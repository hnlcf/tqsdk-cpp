import { For, Show, createSignal, onCleanup } from "solid-js";
import type { Quote } from "../api/client";
import type { WatchlistGroup } from "../lib/storage";
import { instrumentName, normalizeInstrumentId } from "../lib/instruments";
import { formatPrice, priceDecimals, quoteChange } from "../lib/format";

export interface InstrumentSuggestion {
  symbol: string;
  name: string;
  exchange: string;
}

interface Props {
  groups: WatchlistGroup[];
  activeId: string;
  symbols: string[];
  quotes: Record<string, Quote>;
  selected: string;
  onSelect: (symbol: string) => void;
  onSelectGroup: (id: string) => void;
  onAddGroup: (name: string) => void;
  onRenameGroup: (id: string, name: string) => void;
  onRemoveGroup: (id: string) => void;
  onAdd: (symbol: string) => void;
  onRemove: (symbol: string) => void;
  onMove: (symbol: string, direction: -1 | 1) => void;
  onSearch: (query: string) => Promise<InstrumentSuggestion[]>;
}

export default function WatchlistPanel(props: Props) {
  const [entry, setEntry] = createSignal("");
  const [editing, setEditing] = createSignal(false);
  const [suggestions, setSuggestions] = createSignal<InstrumentSuggestion[]>([]);
  const [searching, setSearching] = createSignal(false);
  const [groupEditing, setGroupEditing] = createSignal(false);
  const [groupName, setGroupName] = createSignal("");
  const [renaming, setRenaming] = createSignal(false);
  const [renameValue, setRenameValue] = createSignal("");
  let searchTimer: number | undefined;

  const activeGroup = () => props.groups.find((group) => group.id === props.activeId);

  onCleanup(() => window.clearTimeout(searchTimer));

  function scheduleSearch(value: string) {
    window.clearTimeout(searchTimer);
    const query = value.trim();
    if (!query) {
      setSuggestions([]);
      setSearching(false);
      return;
    }
    setSearching(true);
    searchTimer = window.setTimeout(async () => {
      const result = await props.onSearch(query);
      setSuggestions(result);
      setSearching(false);
    }, 180);
  }

  function selectSuggestion(symbol: string) {
    props.onAdd(symbol);
    setEntry("");
    setSuggestions([]);
    setEditing(false);
  }

  function submit(event: SubmitEvent) {
    event.preventDefault();
    const symbol = normalizeInstrumentId(entry());
    if (!symbol) return;
    props.onAdd(symbol);
    setEntry("");
    setSuggestions([]);
    setEditing(false);
  }

  function submitGroup(event: SubmitEvent) {
    event.preventDefault();
    props.onAddGroup(groupName());
    setGroupName("");
    setGroupEditing(false);
  }

  function submitRename(event: SubmitEvent) {
    event.preventDefault();
    props.onRenameGroup(props.activeId, renameValue());
    setRenaming(false);
  }

  return (
    <aside class="panel watchlist">
      <div class="panel-heading">
        <div>
          <span class="eyebrow">WATCHLISTS</span>
          <h2>{activeGroup()?.name ?? "自选合约"}</h2>
        </div>
        <button
          class="icon-button"
          type="button"
          title="添加合约"
          onClick={() => setEditing((value) => !value)}
        >
          {editing() ? "×" : "+"}
        </button>
      </div>

      <div class="watchlist-groups">
        <select
          value={props.activeId}
          onChange={(event) => props.onSelectGroup(event.currentTarget.value)}
          aria-label="切换自选列表"
        >
          <For each={props.groups}>
            {(group) => (
              <option value={group.id}>
                {group.name} ({group.symbols.length})
              </option>
            )}
          </For>
        </select>
        <button type="button" title="新建列表" onClick={() => setGroupEditing((value) => !value)}>
          +
        </button>
        <button type="button" title="重命名列表" onClick={() => {
          setRenaming((value) => !value);
          setRenameValue(activeGroup()?.name ?? "");
        }}>
          ✎
        </button>
        <button
          type="button"
          title="删除列表"
          disabled={props.groups.length <= 1}
          onClick={() => props.onRemoveGroup(props.activeId)}
        >
          ×
        </button>
      </div>

      <Show when={groupEditing()}>
        <form class="watch-add group-add" onSubmit={submitGroup}>
          <input
            autofocus
            value={groupName()}
            placeholder="列表名称，例如 黑色系"
            onInput={(event) => setGroupName(event.currentTarget.value)}
          />
          <button type="submit">创建</button>
        </form>
      </Show>

      <Show when={renaming()}>
        <form class="watch-add group-add" onSubmit={submitRename}>
          <input
            autofocus
            value={renameValue()}
            placeholder="列表名称"
            onInput={(event) => setRenameValue(event.currentTarget.value)}
          />
          <button type="submit">保存</button>
        </form>
      </Show>

      <Show when={editing()}>
        <form class="watch-add" onSubmit={submit}>
          <input
            autofocus
            value={entry()}
            placeholder="代码/名称，例如 rb 或 螺纹"
            onInput={(event) => {
              setEntry(event.currentTarget.value);
              scheduleSearch(event.currentTarget.value);
            }}
          />
          <button type="submit" title="直接加入；目录搜索无结果时可输入完整代码">
            加入
          </button>
        </form>
        <div class="watch-suggestions">
          <Show when={searching()}>
            <div class="empty-state small">搜索合约目录...</div>
          </Show>
          <For each={suggestions()}>
            {(item) => (
              <button
                type="button"
                class="watch-suggestion"
                onClick={() => selectSuggestion(item.symbol)}
              >
                <span>
                  <strong>{item.symbol}</strong>
                  <small>{item.name || instrumentName(item.symbol)}</small>
                </span>
                <span class="exchange-tag">{item.exchange}</span>
              </button>
            )}
          </For>
          <Show when={!searching() && entry().trim() && suggestions().length === 0}>
            <div class="empty-state small">
              没有匹配结果；输入完整代码后可直接“加入”。
            </div>
          </Show>
        </div>
      </Show>

      <div class="watchlist-items">
        <For each={props.symbols}>
          {(symbol) => {
            const quote = () => props.quotes[symbol];
            const change = () => quoteChange(quote());
            return (
              <div
                class={`watch-row ${props.selected === symbol ? "active" : ""}`}
                onClick={() => props.onSelect(symbol)}
              >
                <span class="watch-main">
                  <strong>{symbol}</strong>
                  <small>{quote()?.instrument_name ?? instrumentName(symbol)}</small>
                </span>
                <span class="watch-price">
                  <strong>{formatPrice(quote()?.last_price, priceDecimals(quote(), 2))}</strong>
                  <small class={change().pct >= 0 ? "positive" : "negative"}>
                    {change().pct >= 0 ? "+" : ""}
                    {change().pct.toFixed(2)}%
                  </small>
                </span>
                <span class="watch-actions" onClick={(event) => event.stopPropagation()}>
                  <button type="button" title="上移" onClick={() => props.onMove(symbol, -1)}>
                    ↑
                  </button>
                  <button type="button" title="下移" onClick={() => props.onMove(symbol, 1)}>
                    ↓
                  </button>
                  <button type="button" title="移除" onClick={() => props.onRemove(symbol)}>
                    ×
                  </button>
                </span>
              </div>
            );
          }}
        </For>
      </div>

      <div class="session-note">
        <span class="session-bar" />
        <div>
          <strong>{props.groups.length} 个自选列表</strong>
          <small>代码/名称模糊搜索 · 本地持久化</small>
        </div>
      </div>
    </aside>
  );
}
