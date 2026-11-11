import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createMarketStream } from "./client";

class FakeSocket {
  static readonly OPEN = 1;
  static readonly CLOSED = 3;
  static instances: FakeSocket[] = [];

  readyState = 0;
  sent: string[] = [];
  onmessage: ((event: { data: string }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;

  constructor(readonly url: string) {
    FakeSocket.instances.push(this);
  }

  open() {
    this.readyState = FakeSocket.OPEN;
  }

  send(data: string) {
    this.sent.push(data);
  }

  message(value: unknown) {
    this.onmessage?.({ data: JSON.stringify(value) });
  }

  close() {
    this.readyState = FakeSocket.CLOSED;
    this.onclose?.();
  }

  fail() {
    this.onerror?.();
  }
}

describe("market stream", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    FakeSocket.instances = [];
    vi.stubGlobal("WebSocket", FakeSocket);
    vi.stubGlobal("window", {
      setTimeout,
      clearTimeout,
      setInterval,
      clearInterval,
      location: { protocol: "http:", host: "127.0.0.1:5175" },
    });
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("keeps account scope in the WebSocket URL", () => {
    const stream = createMarketStream({}, "B");
    expect(FakeSocket.instances).toHaveLength(1);
    expect(FakeSocket.instances[0].url).toContain("/api/stream?account=B");
    stream.close();
  });

  it("reconnects with backoff and re-subscribes after welcome", () => {
    const statuses: string[] = [];
    const stream = createMarketStream(
      {
        onStatus: (status) => statuses.push(status),
      },
      "A",
    );
    const first = FakeSocket.instances[0];
    first.open();
    first.message({ type: "welcome", protocol: 1 });
    expect(statuses).toContain("online");
    expect(first.sent[0]).toContain('"op":"subscribe"');

    first.close();
    expect(statuses.at(-1)).toBe("offline");
    vi.advanceTimersByTime(300);
    expect(FakeSocket.instances).toHaveLength(2);
    expect(FakeSocket.instances[1].url).toContain("account=A");

    const second = FakeSocket.instances[1];
    second.open();
    second.message({ type: "welcome", protocol: 1 });
    expect(second.sent[0]).toContain('"op":"subscribe"');
    stream.close();
  });

  it("closing one account stream does not reconnect it", () => {
    const a = createMarketStream({}, "A");
    const b = createMarketStream({}, "B");
    a.close();
    vi.advanceTimersByTime(10_000);
    expect(FakeSocket.instances).toHaveLength(2);
    b.close();
  });
});
