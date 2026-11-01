#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["websockets"]
# ///
"""最小探测：连接天勤 free-openmd 行情，确认登录/订阅/rtn_data 结构。

运行：uv run scripts/probe_md.py
"""
import asyncio
import json
import ssl

import websockets

URL = "wss://free-openmd.shinnytech.com/t/md/front/mobile"


async def probe(do_login: bool):
    ssl_ctx = ssl.create_default_context()
    tag = "WITH_LOGIN" if do_login else "NO_LOGIN"
    print(f"\n===== 探测 [{tag}] {URL} =====", flush=True)
    async with websockets.connect(URL, ssl=ssl_ctx, max_size=2**24) as ws:
        print("websocket connected", flush=True)

        if do_login:
            await ws.send(json.dumps({"aid": "req_login",
                                      "bid": "9999",
                                      "user_name": "",
                                      "password": ""}))
            print("sent req_login(empty)", flush=True)

        await ws.send(json.dumps({"aid": "subscribe_quote",
                                  "ins_list": "SHFE.rb2610,SHFE.rb"}))
        print("sent subscribe_quote", flush=True)
        await ws.send(json.dumps({"aid": "peek_message"}))
        print("sent peek_message", flush=True)

        rtn_count = 0
        got_quote = False
        for i in range(40):
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=8)
            except asyncio.TimeoutError:
                print("recv timeout", flush=True)
                break
            obj = json.loads(raw)
            aid = obj.get("aid")
            if aid == "rtn_data":
                rtn_count += 1
                data = obj.get("data", [])
                print(f"[frame {i}] rtn_data data_len={len(data)}", flush=True)
                for patch in data:
                    q = patch.get("quotes")
                    if q:
                        got_quote = True
                        for sym, fields in q.items():
                            keys = sorted(fields.keys())
                            print(f"  quote {sym}: keys={keys}", flush=True)
                            print(f"    sample: last_price={fields.get('last_price')} "
                                  f"bid_price1={fields.get('bid_price1')} "
                                  f"amount={fields.get('amount')} "
                                  f"close={fields.get('close')!r}", flush=True)
                    else:
                        print(f"  patch keys={sorted(patch.keys())}", flush=True)
                await ws.send(json.dumps({"aid": "peek_message"}))
            else:
                print(f"[frame {i}] aid={aid} -> {json.dumps(obj, ensure_ascii=False)[:200]}",
                      flush=True)
                await ws.send(json.dumps({"aid": "peek_message"}))
            if got_quote and rtn_count >= 3:
                break

        print(f"RESULT [{tag}]: rtn_data_frames={rtn_count} got_quote={got_quote}", flush=True)
        return got_quote


async def main():
    ok = False
    try:
        ok = await probe(False)
    except Exception as e:
        print(f"NO_LOGIN probe failed: {e!r}", flush=True)
    if not ok:
        try:
            await probe(True)
        except Exception as e:
            print(f"WITH_LOGIN probe failed: {e!r}", flush=True)
    print("\nDONE", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
