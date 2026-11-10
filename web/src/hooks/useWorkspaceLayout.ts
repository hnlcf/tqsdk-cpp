import { createEffect, createSignal, onCleanup } from "solid-js";
import {
  readWorkspaceLayout,
  saveWorkspaceLayout,
  type WorkspaceLayout,
} from "../lib/storage";

export type ResizeKind = "watchlist" | "rightRail" | "table";

export const LAYOUT_LIMITS = {
  watchlistWidth: { min: 180, max: 420 },
  rightRailWidth: { min: 220, max: 520 },
  tableHeight: { min: 120, max: 520 },
} as const;

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, Math.round(value)));
}

/** Pure resize math, separated for unit tests. */
export function applyResize(
  kind: ResizeKind,
  dx: number,
  dy: number,
  start: WorkspaceLayout,
): WorkspaceLayout {
  if (kind === "watchlist") {
    return {
      ...start,
      watchlistWidth: clamp(
        start.watchlistWidth + dx,
        LAYOUT_LIMITS.watchlistWidth.min,
        LAYOUT_LIMITS.watchlistWidth.max,
      ),
    };
  }
  if (kind === "rightRail") {
    return {
      ...start,
      rightRailWidth: clamp(
        start.rightRailWidth - dx,
        LAYOUT_LIMITS.rightRailWidth.min,
        LAYOUT_LIMITS.rightRailWidth.max,
      ),
    };
  }
  return {
    ...start,
    tableHeight: clamp(
      start.tableHeight + dy,
      LAYOUT_LIMITS.tableHeight.min,
      LAYOUT_LIMITS.tableHeight.max,
    ),
  };
}

/**
 * Resizable workspace state + localStorage persistence.
 *
 * Pointer listeners are installed only while dragging and removed on
 * pointerup/pointercancel or component cleanup.
 */
export function useWorkspaceLayout() {
  const [layout, setLayout] = createSignal<WorkspaceLayout>(readWorkspaceLayout());
  let resize: {
    kind: ResizeKind;
    startX: number;
    startY: number;
    startLayout: WorkspaceLayout;
  } | null = null;

  const resizeMove = (event: PointerEvent) => {
    if (!resize) return;
    setLayout(
      applyResize(
        resize.kind,
        event.clientX - resize.startX,
        event.clientY - resize.startY,
        resize.startLayout,
      ),
    );
  };

  const stopResize = () => {
    if (!resize) return;
    resize = null;
    window.removeEventListener("pointermove", resizeMove);
    window.removeEventListener("pointerup", stopResize);
    window.removeEventListener("pointercancel", stopResize);
    saveWorkspaceLayout(layout());
  };

  const startResize = (kind: ResizeKind, event: PointerEvent) => {
    event.preventDefault();
    stopResize();
    resize = {
      kind,
      startX: event.clientX,
      startY: event.clientY,
      startLayout: layout(),
    };
    window.addEventListener("pointermove", resizeMove);
    window.addEventListener("pointerup", stopResize);
    window.addEventListener("pointercancel", stopResize);
  };

  createEffect(() => saveWorkspaceLayout(layout()));
  onCleanup(() => {
    window.removeEventListener("pointermove", resizeMove);
    window.removeEventListener("pointerup", stopResize);
    window.removeEventListener("pointercancel", stopResize);
  });

  return { layout, startResize };
}
