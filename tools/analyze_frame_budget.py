#!/usr/bin/env python3
"""Summarize BGI_TRACE_GPU_FRAMES logs without counting vsync as CPU work.

Writes every timed main-loop frame to CSV and a compact JSON summary.
Callback timings can be nested; their costs must not be summed together.
"""
import argparse
import csv
import json
import math
import re
from pathlib import Path


def fields(line):
    return {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}


def summary(rows, key="total_ns"):
    values = sorted(row[key] / 1e6 for row in rows)
    if not values:
        return {"frames": 0}
    return {
        "frames": len(values),
        "median_ms": round(values[len(values) // 2], 3),
        "p95_ms": round(values[math.ceil(len(values) * .95) - 1], 3),
        "max_ms": round(values[-1], 3),
        "over_16_667_ms": sum(v > 1000 / 60 for v in values),
        "over_20_ms": sum(v > 20 for v in values),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="Output prefix")
    parser.add_argument("--scene-tick", type=int, help="SDL tick at start of measured scene")
    parser.add_argument("--scene-elapsed-ms", type=int,
                        help="Input-loop elapsed time at scene start (independent of startup/shader compilation)")
    args = parser.parse_args()
    rows, callbacks, standalone = [], [], []
    render = None
    for line in args.log.read_text(errors="replace").splitlines():
        if line.startswith("GPU_CALLBACK_COST "):
            callbacks.append(fields(line))
        elif line.startswith("GPU_FRAME_COST "):
            if render is not None:
                standalone.append(render)
            render = fields(line)
            render["callbacks"] = callbacks
            callbacks = []
        elif line.startswith("FRAME_BUDGET "):
            row = fields(line)
            if render is not None:
                row.update({"gpu_" + k: v for k, v in render.items() if k != "callbacks"})
                row["callbacks"] = render["callbacks"]
            render = None
            row["work_ns"] = row["total_ns"] - row.get("gpu_present_ns", 0) - row["pacing_ns"]
            rows.append(row)
    if render is not None:
        standalone.append(render)
    if not rows:
        parser.error("No FRAME_BUDGET records; enable BGI_TRACE_GPU_FRAMES=1")
    columns = sorted({key for row in rows for key in row if key != "callbacks"})
    with args.output.with_suffix(".csv").open("w") as out:
        writer = csv.DictWriter(out, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    top = sorted(rows, key=lambda row: row["total_ns"], reverse=True)[:20]
    result = {
        "log": str(args.log),
        "budget_ms": 1000 / 60,
        "all_frames": summary(rows),
        "work_excluding_present_and_pacing": summary(rows, "work_ns"),
        "changed_frames": summary([r for r in rows if r.get("gpu_kernel_calls", 0)]),
        "slowest": top,
        "standalone_render_frames": standalone,
        "notes": [
            "Full loop includes VM/input/render and vsync wait; tiny excesses near 16.67 ms can be scheduling jitter.",
            "work_ns excludes blocking SDL_RenderPresent and explicit pacing; includes synchronous GPU waits elsewhere.",
            "GPU_CALLBACK_COST can be nested; do not sum callback durations.",
            "Initial render and final render at runtime limit may lack a FRAME_BUDGET record; listed separately.",
            "Instrumentation/logging adds overhead; results describe this run and are not Android hardware measurements.",
        ],
    }
    if args.scene_tick is not None or args.scene_elapsed_ms is not None:
        if args.scene_elapsed_ms is not None:
            # Older logs lack elapsed_ms. The first complete loop is the
            # closest available estimate of the input-loop origin.
            origin = rows[0]["tick"] - rows[0]["total_ns"] / 1e6
            scene = [r for r in rows if r.get("elapsed_ms", r["tick"] - origin) >= args.scene_elapsed_ms]
            result["scene_elapsed_ms"] = args.scene_elapsed_ms
            result["scene_elapsed_estimated"] = not all("elapsed_ms" in r for r in rows)
        else:
            scene = [r for r in rows if r["tick"] >= args.scene_tick]
        changed = [r for r in scene if r.get("gpu_kernel_calls", 0)]
        if args.scene_tick is not None:
            result["scene_tick"] = args.scene_tick
        result["scene"] = summary(scene)
        result["scene_work"] = summary(scene, "work_ns")
        result["scene_changed"] = summary(changed)
        result["scene_slowest"] = sorted(scene, key=lambda r: r["total_ns"], reverse=True)[:10]
        by_object = {}
        for row in changed:
            for callback in row.get("callbacks", []):
                by_object.setdefault(callback["object"], []).append(callback)
        result["scene_callbacks"] = {
            hex(obj): summary(costs) for obj, costs in by_object.items()
        }
    args.output.with_suffix(".json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in (
        "slowest", "scene_slowest", "standalone_render_frames", "notes")}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
