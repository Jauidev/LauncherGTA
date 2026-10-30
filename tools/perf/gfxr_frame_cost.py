"""Estimates the tile-memory cost of every render pass in a gfxrecon-convert jsonl capture.

For each vkCmdBeginRenderPass: render area, attachments (format, samples), load/store ops from the
render pass, draws and clears inside. Load traffic = area * samples * bpp for LOAD attachments,
store traffic the same for STORE attachments (what a tiler reads/writes from DRAM).
Usage: py -3 -I frame_cost.py gta4.jsonl
"""
import collections
import json
import sys

BPP = {"R8G8B8A8_UNORM": 4, "B8G8R8A8_UNORM": 4, "A2B10G10R10_UNORM_PACK32": 4, "R16G16B16A16_SFLOAT": 8,
       "R32G32_SFLOAT": 8, "D32_SFLOAT_S8_UINT": 5, "D24_UNORM_S8_UINT": 4, "R16G16B16A16_UNORM": 8,
       "R32_SFLOAT": 4, "R16G16_SFLOAT": 4, "R32G32B32A32_SFLOAT": 16, "R16G16_SNORM": 4,
       "R16G16B16A16_SNORM": 8, "R8G8_UNORM": 2, "R8_UNORM": 1, "R32_UINT": 4, "R16G16_UNORM": 4,
       "R32G32_UINT": 8, "R8G8B8A8_SRGB": 4, "B10G11R11_UFLOAT_PACK32": 4, "A2R10G10B10_UNORM_PACK32": 4}

images, views, framebuffers, renderpasses = {}, {}, {}, {}
open_pass = {}
scissor = {}  # command buffer -> (x0, y0, x1, y1)

def grow(p, r):
    x0, y0, x1, y1 = r
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, p["w"]), min(y1, p["h"])
    if x1 <= x0 or y1 <= y0:
        return
    u = p.get("u")
    p["u"] = (x0, y0, x1, y1) if not u else (min(u[0], x0), min(u[1], y0), max(u[2], x1), max(u[3], y1))

frames = [[]]
dispatches = [0]
barriers = [0]
copies = [0]

def short(v):
    return str(v).replace("VK_FORMAT_", "").replace("VK_SAMPLE_COUNT_", "").replace("_BIT", "")

for line in open(sys.argv[1], encoding="utf-8"):
    try:
        o = json.loads(line)
    except json.JSONDecodeError:
        continue
    f = o.get("function")
    if not f:
        continue
    name, a = f["name"], f.get("args", {})
    if name == "vkCreateImage":
        i = a["pCreateInfo"]
        images[a["pImage"]] = (short(i["format"]), int(short(i["samples"])))
    elif name == "vkCreateImageView":
        views[a["pView"]] = a["pCreateInfo"]["image"]
    elif name == "vkCreateFramebuffer":
        framebuffers[a["pFramebuffer"]] = a["pCreateInfo"].get("pAttachments") or []
    elif name in ("vkCreateRenderPass", "vkCreateRenderPass2", "vkCreateRenderPass2KHR"):
        atts = a["pCreateInfo"].get("pAttachments") or []
        renderpasses[a["pRenderPass"]] = [
            (short(x["format"]), int(short(x["samples"])), x["loadOp"].split("_")[-1],
             x["storeOp"].split("_")[-1], x["stencilLoadOp"].split("_")[-1],
             x["stencilStoreOp"].split("_")[-1]) for x in atts]
    elif name == "vkCmdBeginRenderPass":
        b = a["pRenderPassBegin"]
        ext = b["renderArea"]["extent"]
        open_pass[a["commandBuffer"]] = {"rp": b["renderPass"], "w": ext["width"], "h": ext["height"],
                                         "draws": 0, "clears": 0}
    elif name == "vkCmdSetScissor":
        s = a["pScissors"][0]
        scissor[a["commandBuffer"]] = (s["offset"]["x"], s["offset"]["y"], s["offset"]["x"] + s["extent"]["width"],
                                       s["offset"]["y"] + s["extent"]["height"])
    elif name in ("vkCmdDraw", "vkCmdDrawIndexed"):
        p = open_pass.get(a["commandBuffer"])
        if p:
            p["draws"] += 1
            grow(p, scissor.get(a["commandBuffer"], (0, 0, 1 << 30, 1 << 30)))
    elif name == "vkCmdClearAttachments":
        p = open_pass.get(a["commandBuffer"])
        if p:
            p["clears"] += 1
            for r in a["pRects"]:
                o, e = r["rect"]["offset"], r["rect"]["extent"]
                grow(p, (o["x"], o["y"], o["x"] + e["width"], o["y"] + e["height"]))
    elif name == "vkCmdEndRenderPass":
        p = open_pass.pop(a["commandBuffer"], None)
        if p:
            frames[-1].append(p)
    elif name == "vkCmdDispatch":
        dispatches[-1] += 1
    elif name == "vkCmdPipelineBarrier":
        barriers[-1] += 1
    elif name in ("vkCmdCopyBufferToImage", "vkCmdCopyBuffer", "vkCmdCopyImage", "vkCmdCopyImageToBuffer"):
        copies[-1] += 1
    elif name == "vkQueuePresentKHR":
        frames.append([]); dispatches.append(0); barriers.append(0); copies.append(0)

for fi, passes in enumerate(frames):
    if len(passes) < 20:
        continue
    tot = collections.Counter()
    kinds = collections.Counter()
    rows = []
    for p in passes:
        atts = renderpasses.get(p["rp"], [])
        px = p["w"] * p["h"]
        u = p.get("u") or (0, 0, 0, 0)
        tpx = (u[2] - u[0]) * (u[3] - u[1])
        load = store = tload = tstore = 0
        desc = []
        for fmt, s, lo, st, slo, sst in atts:
            bpp = BPP.get(fmt, 4)
            if lo == "LOAD" or slo == "LOAD":
                load += px * s * bpp
                tload += tpx * s * bpp
            if st == "STORE" or sst == "STORE":
                store += px * s * bpp
                tstore += tpx * s * bpp
            desc.append(f"{fmt}x{s} {lo[0]}{st[0]}")
        kind = "game" if p["draws"] >= 3 else ("clear" if p["draws"] == 0 else "xfer/1-2 draws")
        kinds[kind] += 1
        tot[kind + " load"] += load
        tot[kind + " store"] += store
        tot[kind + " tight load+store"] += tload + tstore
        tot[kind + " shaded px"] += px * max(s for _, s, *_ in atts) if atts else 0
        rows.append((load + store, p, desc, kind))
    print(f"=== frame {fi}: {len(passes)} passes {dict(kinds)}, dispatches {dispatches[fi]}, "
          f"barriers {barriers[fi]}, copies {copies[fi]}")
    for k in sorted(tot):
        print(f"  {k:28} {tot[k]/1e6:9.1f} MB")
    print(f"  TOTAL load+store            {sum(v for k, v in tot.items() if 'px' not in k and 'tight' not in k)/1e6:9.1f} MB")
    print(f"  TOTAL tight                 {sum(v for k, v in tot.items() if 'tight' in k)/1e6:9.1f} MB")
    print("  top passes by load+store:")
    for cost, p, desc, kind in sorted(rows, key=lambda r: -r[0])[:25]:
        print(f"   {cost/1e6:7.1f} MB {p['w']}x{p['h']} used {p.get('u')} draws {p['draws']:3} clears {p['clears']} [{kind}] {' | '.join(desc)}")
