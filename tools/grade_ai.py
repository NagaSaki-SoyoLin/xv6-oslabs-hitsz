#!/usr/bin/env python3
"""运行 Lab3 AI 双通路并按同机相对倍率汇总指标。"""

import argparse
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path


METRICS_RE = re.compile(
    r"^AIINFER_METRICS mode=(?P<mode>baseline|student|prefetch) "
    r"workers=(?P<workers>[1-9][0-9]*) requests=(?P<requests>[1-9][0-9]*) "
    r"elapsed_ticks=(?P<elapsed_ticks>[1-9][0-9]*) "
    r"throughput_milli=(?P<throughput_milli>[1-9][0-9]*) "
    r"p95_ticks=(?P<p95_ticks>[0-9]+) "
    r"kmem_spins=(?P<kmem_spins>[0-9]+) "
    r"bcache_spins=(?P<bcache_spins>[0-9]+) "
    r"total_spins=(?P<total_spins>[0-9]+) "
    r"model_read_bytes=(?P<model_read_bytes>[1-9][0-9]*) "
    r"embed_read_bytes=(?P<embed_read_bytes>[1-9][0-9]*) "
    r"kv_write_bytes=(?P<kv_write_bytes>[1-9][0-9]*) "
    r"kv_read_bytes=(?P<kv_read_bytes>[1-9][0-9]*) "
    r"checksum=(?P<checksum>[1-9][0-9]*)$"
)

DEFAULT_POLICY = {
    "runs": 3,
    "workers": 3,
    "requests": 24,
    "kv_file_bytes": 2048,
    "weights": {"throughput": 0.5, "spins": 0.3, "p95": 0.2},
    "caps": {"throughput": 2.0, "spins": 8.0, "p95": 2.0},
}


def run_checked(command, cwd, timeout):
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(
            "命令执行失败：{}\n{}".format(" ".join(command), completed.stdout[-5000:])
        )
    return completed.stdout


def parse_metrics(output_path, mode):
    lines = output_path.read_text(encoding="utf-8", errors="replace").splitlines()
    success = "AIINFER: verify OK mode=" + mode
    if success not in lines:
        raise ValueError("缺少成功标记：" + success)
    matches = [METRICS_RE.fullmatch(line) for line in lines]
    matches = [match for match in matches if match is not None]
    if len(matches) != 1:
        raise ValueError("输出中必须恰好包含一条完整指标行")
    if matches[0].group("mode") != mode:
        raise ValueError("指标模式与运行模式不一致")
    return {
        key: int(value)
        for key, value in matches[0].groupdict().items()
        if key != "mode"
    }


def run_mode(root, mode, timeout):
    run_checked(["./grade-ai", mode], root, timeout)
    return parse_metrics(root / ("ai-" + mode + ".out"), mode)


def validate_pair(baseline, student, policy):
    expected_kv = policy["workers"] * policy["requests"] * policy["kv_file_bytes"]
    minimum_shards = policy["workers"] * 37 * 1024
    for metrics in (baseline, student):
        if metrics["workers"] != policy["workers"]:
            raise ValueError("worker 数量与评分策略不一致")
        if metrics["requests"] != policy["requests"]:
            raise ValueError("请求数量与评分策略不一致")
        if metrics["kv_write_bytes"] != expected_kv:
            raise ValueError("KV 写入字节数不正确")
        if metrics["kv_read_bytes"] != expected_kv:
            raise ValueError("KV 读取字节数不正确")
        if metrics["model_read_bytes"] < minimum_shards:
            raise ValueError("模型文件读取量低于完整工作集")
        if metrics["embed_read_bytes"] < minimum_shards:
            raise ValueError("embedding 文件读取量低于完整工作集")
    if baseline["checksum"] != student["checksum"]:
        raise ValueError("baseline 与 student 输出校验和不一致")


def median_metrics(samples):
    return {
        field: statistics.median(sample[field] for sample in samples)
        for field in samples[0]
    }


def ratios(baseline, optimized):
    return {
        "throughput": optimized["throughput_milli"] / baseline["throughput_milli"],
        "spins": baseline["total_spins"] / max(1, optimized["total_spins"]),
        "p95": baseline["p95_ticks"] / max(1, optimized["p95_ticks"]),
    }


def weighted_ratio(values, policy):
    capped = {
        name: min(values[name], policy["caps"][name])
        for name in policy["weights"]
    }
    score = sum(capped[name] * policy["weights"][name] for name in capped)
    return capped, score


def load_policy(path):
    policy = dict(DEFAULT_POLICY)
    if path is not None:
        policy.update(json.loads(path.read_text(encoding="utf-8")))
    return policy


def main(argv=None):
    parser = argparse.ArgumentParser(description="评测 Lab3 AI 相对性能")
    parser.add_argument("--runs", type=int)
    parser.add_argument("--timeout", type=int, default=420)
    parser.add_argument("--policy", type=Path)
    parser.add_argument("--bonus", action="store_true")
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[1]
    policy = load_policy(args.policy)
    runs = args.runs or policy["runs"]
    if runs < 1:
        raise ValueError("runs 必须是正整数")

    baseline_samples = []
    student_samples = []
    bonus_samples = []
    for _ in range(runs):
        baseline = run_mode(root, "baseline", args.timeout)
        student = run_mode(root, "student", args.timeout)
        validate_pair(baseline, student, policy)
        baseline_samples.append(baseline)
        student_samples.append(student)
        if args.bonus:
            bonus = run_mode(root, "prefetch", args.timeout)
            validate_pair(student, bonus, policy)
            bonus_samples.append(bonus)

    baseline_median = median_metrics(baseline_samples)
    student_median = median_metrics(student_samples)
    required_ratios = ratios(baseline_median, student_median)
    capped, score = weighted_ratio(required_ratios, policy)
    result = {
        "runs": runs,
        "baseline_samples": baseline_samples,
        "student_samples": student_samples,
        "baseline_median": baseline_median,
        "student_median": student_median,
        "required_ratios": required_ratios,
        "capped_ratios": capped,
        "weighted_ratio": score,
    }
    if args.bonus:
        bonus_median = median_metrics(bonus_samples)
        result["prefetch_samples"] = bonus_samples
        result["prefetch_median"] = bonus_median
        result["bonus_ratios"] = ratios(student_median, bonus_median)
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print("grade_ai：" + str(error), file=sys.stderr)
        sys.exit(1)
