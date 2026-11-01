#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["websockets"]
# ///
"""探测天勤 K 线：set_chart 订阅 -> rtn_data.klines 结构。
运行：uv run scripts/probe_kline.py

关键结论（经真实连接确认）：
  - chart_id 必须是 32 位小写十六进制（对齐官方 TqSdk _generate_uuid()）；
    含 @ / . 等特殊字符会被服务器直接断连。
  - set_chart 参数：chart_id / ins_list / duration(ns) / view_width。
  - rtn_data.klines 形态：{sym: {duration_ns: {"data": {"<bar_id>": bar, ...}}}}
    （首帧给全量历史；后续增量给单根；纯元信息帧无 "data"）。
  - 取消订阅：ins_list=""。
"""

import asyncio
import json
import secrets
import ssl

import websockets

URL = "wss://free-openmd.shinnytech.com/t/md/front/mobile"


def gen_chart_id() -> str:
    # 对齐官方 TqSdk：f"{getrandbits(128):032x}"
    return f"{secrets.randbits(128):032x}"


async def main():
    ctx = ssl.create_default_context()
    chart_id = gen_chart_id()
    async with websockets.connect(URL, ssl=ctx, max_size=2**24) as ws:
        print("connected", flush=True)

        # 先订阅行情（保证合约被加载），再 set_chart 订阅 60s K 线。
        await ws.send(json.dumps({"aid": "subscribe_quote", "ins_list": "SHFE.rb2610"}))
        await ws.send(
            json.dumps(
                {
                    "aid": "set_chart",
                    "chart_id": chart_id,
                    "ins_list": "SHFE.rb2610",
                    "duration": 60_000_000_000,
                    "view_width": 20,
                }
            )
        )
        await ws.send(json.dumps({"aid": "peek_message"}))
        print(f"sent subscribe_quote + set_chart({chart_id}) + peek", flush=True)

        got_kline = False
        for i in range(60):
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=8)
            except asyncio.TimeoutError:
                print("recv timeout", flush=True)
                break
            obj = json.loads(raw)
            aid = obj.get("aid")
            if aid != "rtn_data":
                print(
                    f"[{i}] aid={aid} {json.dumps(obj, ensure_ascii=False)[:150]}",
                    flush=True,
                )
                await ws.send(json.dumps({"aid": "peek_message"}))
                continue

            data = obj.get("data", [])
            for patch in data:
                kl = patch.get("klines")
                if kl:
                    got_kline = True
                    print(f"[{i}] KLINES patch:", flush=True)
                    for chart_or_sym, inner in kl.items():
                        print(
                            f"   key={chart_or_sym!r} type={type(inner).__name__}",
                            flush=True,
                        )
                        print(
                            f"   inner={json.dumps(inner, ensure_ascii=False)[:600]}",
                            flush=True,
                        )
                # chart 元信息（set_chart 回执）
                for k in patch.keys():
                    if "chart" in k.lower() and k != "klines":
                        print(
                            f"[{i}] {k} = {json.dumps(patch[k], ensure_ascii=False)[:300]}",
                            flush=True,
                        )
            await ws.send(json.dumps({"aid": "peek_message"}))
            if got_kline and i > 5:
                break

        print(f"RESULT got_kline={got_kline}", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
