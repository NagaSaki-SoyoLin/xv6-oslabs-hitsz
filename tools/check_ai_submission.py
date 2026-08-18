#!/usr/bin/env python3
"""检查学生提交是否只修改 Lab3 允许的文件。"""

import subprocess
import sys
from pathlib import Path


ALLOWED = {
    "kernel/kalloc.c",
    "kernel/bio.c",
    "user/ai_student.c",
    "user/ai_student_model.c",
    "user/ai_student_kv.c",
    "user/ai_student_prefetch.c",
    "time.txt",
}


def git_lines(root, *arguments):
    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(completed.stderr.strip())
    return {line.strip() for line in completed.stdout.splitlines() if line.strip()}


def main():
    root = Path(__file__).resolve().parents[1]
    base = "lab3-ai-starter-v1"
    committed = git_lines(root, "diff", "--name-only", base + "..HEAD")
    working = git_lines(root, "diff", "--name-only")
    staged = git_lines(root, "diff", "--cached", "--name-only")
    untracked = git_lines(root, "ls-files", "--others", "--exclude-standard")
    changed = committed | working | staged | untracked
    forbidden = sorted(changed - ALLOWED)
    if forbidden:
        print("提交检查失败：以下文件不允许修改：", file=sys.stderr)
        for path in forbidden:
            print("  " + path, file=sys.stderr)
        return 1
    print("提交检查通过：修改范围符合 Lab3 AI 实验要求。")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print("提交检查失败：" + str(error), file=sys.stderr)
        sys.exit(1)
