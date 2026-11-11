#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
volclava应用级检查点演示程序
===================

功能说明：
    这是一个演示应用级检查点 (Application-Level Checkpoint) 的示例程序。
    模拟一个长时间运行的计算任务，支持以下两种检查点触发方式：

    1. 信号触发模式 (Signal Mode)：
       - 捕获 SIGUSR1: 执行检查点并继续运行
       - 捕获 SIGUSR2: 执行检查点并退出 (checkpoint-and-exit)
       由 echkpnt 程序向目标进程发送信号来触发检查点。

    2. 触发文件模式：
       - 轮询 LSB_CHKPNT_DIR 目录下的 .check 和 .exit 文件
       - .check 触发检查点并继续运行
       - .exit 触发检查点并退出
       由 echkpnt 程序创建触发文件来触发检查点。

检查点文件格式：
    JSON 格式，存储在 checkpoint_dir/checkpoint.json

volclava 集成：
    - 通过环境变量 LSB_CHKPNT_DIR 获取检查点目录
    - 通过环境变量 LSB_JOBID 获取作业 ID
    - 重启时通过 -r 参数表示从检查点恢复

用法：
    首次运行：
        python checkpoint_volclava.py [--steps N] [--interval SECONDS] [--mode signal|file] [checkpoint_dir]

    从检查点恢复：
        python checkpoint_volclava.py -r [checkpoint_dir]

    在 volclava 下运行：
        bsub -k "/path/to/chkpnt_dir method=appcheckpoint" python checkpoint_volclava.py
"""

import os
import sys
import json
import time
import signal
import argparse
import logging
import tempfile

# ============================================================
# 全局配置
# ============================================================

# 检查点文件名
CHECKPOINT_FILENAME = "checkpoint.json"

# 触发文件名（FLUENT 风格）
TRIGGER_CHECK = ".check"
TRIGGER_EXIT = ".exit"

# 检查点退出码（LSF 约定：checkpoint-and-exit 退出码）
# LSF 对 checkpoint-and-exit 的作业，退出码通常为 0 但由 erestart 重新拉起
EXIT_CHECKPOINT = 0

# 默认轮询间隔（秒）
POLL_INTERVAL = 1


# ============================================================
# 日志配置
# ============================================================

def setup_logging():
    """配置日志输出"""
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    return logging.getLogger(__name__)


logger = setup_logging()


# ============================================================
# 应用状态类
# ============================================================

class AppState:
    """
    应用程序状态，封装需要被检查点保存/恢复的数据
    """

    def __init__(self):
        self.step = 0  # 当前步数
        self.total_steps = 600  # 总步数（默认运行约600秒）
        self.counter = 0  # 累加计数器
        self.history = []  # 历史记录
        self.start_time = time.time()  # 启动时间
        self.checkpoint_count = 0  # 检查点次数
        self.signal_history = []  # 收到的信号历史记录

    def to_dict(self):
        """序列化为字典"""
        return {
            "step": self.step,
            "total_steps": self.total_steps,
            "counter": self.counter,
            "history": self.history,
            "checkpoint_count": self.checkpoint_count,
            "signal_history": self.signal_history,
            "elapsed_time": time.time() - self.start_time,
            "checkpoint_time": time.time(),
        }

    @classmethod
    def from_dict(cls, data):
        """从字典反序列化恢复"""
        state = cls()
        state.step = data.get("step", 0)
        state.total_steps = data.get("total_steps", 600)
        state.counter = data.get("counter", 0)
        state.history = data.get("history", [])
        state.checkpoint_count = data.get("checkpoint_count", 0)
        state.signal_history = data.get("signal_history", [])
        # 恢复时重置 start_time，让 elapsed_time 继续累计
        elapsed = data.get("elapsed_time", 0)
        state.start_time = time.time() - elapsed
        return state

    def record_signal(self, signo, signal_name, action):
        """记录收到的信号信息"""
        record = {
            "signal_number": signo,
            "signal_name": signal_name,
            "action": action,  # "checkpoint-and-continue" 或 "checkpoint-and-exit"
            "step": self.step,
            "counter": self.counter,
            "timestamp": time.time(),
            "elapsed_time": time.time() - self.start_time,
        }
        self.signal_history.append(record)

    def __repr__(self):
        return (
            f"AppState(step={self.step}/{self.total_steps}, "
            f"counter={self.counter}, "
            f"checkpoints={self.checkpoint_count}, "
            f"signals_received={len(self.signal_history)})"
        )


# ============================================================
# 检查点管理
# ============================================================

class CheckpointManager:
    """
    检查点管理器
    负责保存和加载应用状态到检查点文件
    """

    def __init__(self, checkpoint_dir):
        self.checkpoint_dir = checkpoint_dir
        self.checkpoint_file = os.path.join(checkpoint_dir, CHECKPOINT_FILENAME)

    def save(self, state: AppState) -> bool:
        """
        原子性地保存检查点
        使用临时文件 + rename 模式，确保崩溃安全
        """
        try:
            os.makedirs(self.checkpoint_dir, exist_ok=True)

            # 写入临时文件
            tmp_fd, tmp_path = tempfile.mkstemp(
                suffix=".tmp",
                prefix=".checkpoint_",
                dir=self.checkpoint_dir,
            )
            try:
                # 先递增计数，再保存（确保检查点文件中的计数正确）
                state.checkpoint_count += 1

                with os.fdopen(tmp_fd, "w") as f:
                    json.dump(state.to_dict(), f, indent=2, ensure_ascii=False)
                    f.flush()
                    os.fsync(f.fileno())

                # 原子性重命名
                os.replace(tmp_path, self.checkpoint_file)

                logger.info(
                    "检查点保存成功: %s (step=%d)",
                    self.checkpoint_file,
                    state.step,
                )
                return True
            except Exception:
                # 清理临时文件
                if os.path.exists(tmp_path):
                    os.unlink(tmp_path)
                raise
        except Exception as e:
            logger.error("保存检查点失败: %s", e)
            return False

    def load(self) -> AppState:
        """
        从检查点文件加载状态"""
        if not os.path.exists(self.checkpoint_file):
            raise FileNotFoundError(f"检查点文件不存在: {self.checkpoint_file}")

        with open(self.checkpoint_file, "r") as f:
            data = json.load(f)

        state = AppState.from_dict(data)
        logger.info(
            "检查点加载成功: %s (step=%d, 已恢复 %d 步)",
            self.checkpoint_file,
            state.step,
            state.step,
        )
        return state

    def exists(self) -> bool:
        """检查检查点文件是否存在"""
        return os.path.exists(self.checkpoint_file)


# ============================================================
# 检查点触发器
# ============================================================

class CheckpointTrigger:
    """
    检查点触发器基类
    """

    def __init__(self, checkpoint_dir, state: AppState, ckpt_mgr: CheckpointManager):
        self.checkpoint_dir = checkpoint_dir
        self.state = state
        self.ckpt_mgr = ckpt_mgr
        self._exit_after_checkpoint = False

    def check(self) -> bool:
        """
        检查是否有检查点请求
        返回 True 表示需要退出（checkpoint-and-exit 模式
        """
        return False

    def do_checkpoint(self, and_exit: bool = False) -> bool:
        """执行检查点操作"""
        success = self.ckpt_mgr.save(self.state)
        if success:
            self._exit_after_checkpoint = and_exit
        return success

    @property
    def should_exit(self):
        return self._exit_after_checkpoint


class SignalTrigger(CheckpointTrigger):
    """
    信号触发模式
    捕获 SIGUSR1 (checkpoint-and-continue)
    捕获 SIGUSR2 (checkpoint-and-exit)
    """

    def __init__(self, checkpoint_dir, state, ckpt_mgr):
        super().__init__(checkpoint_dir, state, ckpt_mgr)
        self._checkpoint_requested = False
        self._exit_requested = False
        self._last_signal = None
        self._setup_signals()

    def _setup_signals(self):
        """设置信号处理器"""
        signal.signal(signal.SIGUSR1, self._sigusr1_handler)
        signal.signal(signal.SIGUSR2, self._sigusr2_handler)
        logger.info("信号触发模式已启用 (SIGUSR1=检查并继续, SIGUSR2=检查并退出)")

    def _sigusr1_handler(self, signum, frame):
        """SIGUSR1 信号处理 - 检查点并继续"""
        sig_name = signal.Signals(signum).name
        elapsed = time.time() - self.state.start_time
        logger.info(
            "收到信号 %s (SIGUSR1), 触发检查点（继续运行）"
            " [step=%d, elapsed=%.2fs]",
            sig_name, self.state.step, elapsed,
        )
        # 记录信号历史
        self.state.record_signal(
            signo=signum,
            signal_name=sig_name,
            action="checkpoint-and-continue",
        )
        self._checkpoint_requested = True
        self._last_signal = sig_name

    def _sigusr2_handler(self, signum, frame):
        """SIGUSR2 信号处理 - 检查点并退出"""
        sig_name = signal.Signals(signum).name
        elapsed = time.time() - self.state.start_time
        logger.info(
            "收到信号 %s (SIGUSR2), 触发检查点（并退出）"
            " [step=%d, elapsed=%.2fs]",
            sig_name, self.state.step, elapsed,
        )
        # 记录信号历史
        self.state.record_signal(
            signo=signum,
            signal_name=sig_name,
            action="checkpoint-and-exit",
        )
        self._checkpoint_requested = True
        self._exit_requested = True
        self._last_signal = sig_name

    def check(self) -> bool:
        """检查是否有待处理的检查点请求"""
        if self._checkpoint_requested:
            self._checkpoint_requested = False
            and_exit = self._exit_requested
            self._exit_requested = False
            self.do_checkpoint(and_exit=and_exit)
            return and_exit
        return False


class FileTrigger(CheckpointTrigger):
    """
    触发文件模式
    轮询 checkpoint_dir 下的 .check 和 .exit 文件
    """

    def __init__(self, checkpoint_dir, state, ckpt_mgr):
        super().__init__(checkpoint_dir, state, ckpt_mgr)
        self.check_file = os.path.join(checkpoint_dir, TRIGGER_CHECK)
        self.exit_file = os.path.join(checkpoint_dir, TRIGGER_EXIT)
        logger.info("文件触发模式已启用 (轮询: %s, %s)", TRIGGER_CHECK, TRIGGER_EXIT)

    def _record_trigger(self, trigger_file, action):
        """记录文件触发事件"""
        elapsed = time.time() - self.state.start_time
        logger.info(
            "检测到触发文件 %s，触发检查点（%s）"
            " [step=%d, elapsed=%.2fs]",
            trigger_file, action, self.state.step, elapsed,
        )
        # 记录到信号历史（文件触发也作为一种触发事件记录）
        self.state.record_signal(
            signo=0,
            signal_name=f"FILE_TRIGGER:{trigger_file}",
            action=action,
        )

    def check(self) -> bool:
        """检查触发文件"""
        # 检查 .exit 文件（优先级更高）
        if os.path.exists(self.exit_file):
            self._record_trigger(TRIGGER_EXIT, "checkpoint-and-exit")
            try:
                os.unlink(self.exit_file)
            except OSError:
                pass
            self.do_checkpoint(and_exit=True)
            return True

        # 检查 .check 文件
        if os.path.exists(self.check_file):
            self._record_trigger(TRIGGER_CHECK, "checkpoint-and-continue")
            try:
                os.unlink(self.check_file)
            except OSError:
                pass
            self.do_checkpoint(and_exit=False)
            return False

        return False


# ============================================================
# 主应用程序
# ============================================================

class CheckpointDemoApp:
    """
    演示应用程序主类
    模拟一个长时间运行的计算任务
    """

    def __init__(self, checkpoint_dir, mode="signal", total_steps=100, interval=1.0):
        self.checkpoint_dir = checkpoint_dir
        self.mode = mode
        self.total_steps = total_steps
        self.interval = interval

        # 初始化状态和检查点管理器
        self.ckpt_mgr = CheckpointManager(checkpoint_dir)
        self.state = AppState()
        self.state.total_steps = total_steps
        self.trigger = None

    def setup_trigger(self):
        """设置检查点触发器"""
        if self.mode == "signal":
            self.trigger = SignalTrigger(
                self.checkpoint_dir, self.state, self.ckpt_mgr
            )
        elif self.mode == "file":
            self.trigger = FileTrigger(
                self.checkpoint_dir, self.state, self.ckpt_mgr)
        else:
            raise ValueError(f"不支持的触发模式: {self.mode}")

    def restore_from_checkpoint(self):
        """从检查点恢复状态"""
        if self.ckpt_mgr.exists():
            self.state = self.ckpt_mgr.load()
            logger.info("从检查点恢复: %s", self.state)
            return True
        return False

    def do_work(self):
        """
        执行一步计算工作（模拟）
        """
        # 模拟一些计算工作
        self.state.step += 1
        self.state.counter += self.state.step
        self.state.history.append({
            "step": self.state.step,
            "counter": self.state.counter,
            "timestamp": time.time(),
        })

        # 每 10 步输出一次进度
        if self.state.step % 10 == 0 or self.state.step == self.state.total_steps:
            elapsed = time.time() - self.state.start_time
            progress = (self.state.step / self.state.total_steps) * 100
            logger.info(
                "进度: %d/%d (%.1f%%) | 计数器=%d | 已用时=%.1fs",
                self.state.step,
                self.state.total_steps,
                progress,
                self.state.counter,
                elapsed,
            )

    def run(self):
        """
        主运行循环
        """
        logger.info("=" * 60)
        logger.info("应用级检查点演示程序启动")
        logger.info("检查点目录: %s", self.checkpoint_dir)
        logger.info("触发模式: %s", self.mode)
        logger.info("作业ID: %s", os.environ.get("LSB_JOBID", "N/A (非 LSF 环境)"))
        logger.info("进程ID: %d", os.getpid())
        logger.info("初始状态: %s", self.state)
        logger.info("=" * 60)

        self.setup_trigger()

        try:
            while self.state.step < self.state.total_steps:
                # 执行一步工作
                self.do_work()

                # 检查检查点触发
                should_exit = self.trigger.check()
                if should_exit:
                    logger.info("检查点完成，准备退出 (checkpoint-and-exit)")
                    return EXIT_CHECKPOINT

                # 休眠
                time.sleep(self.interval)

            # 任务完成
            logger.info("=" * 60)
            logger.info("任务完成!")
            logger.info("最终状态: %s", self.state)
            elapsed = time.time() - self.state.start_time
            logger.info("总用时: %.2f 秒", elapsed)
            logger.info("=" * 60)

            # 保存最终检查点
            self.ckpt_mgr.save(self.state)
            return 0

        except KeyboardInterrupt:
            logger.info("收到中断信号，保存检查点后退出")
            self.ckpt_mgr.save(self.state)
            return 130  # 128 + SIGINT
        except Exception as e:
            logger.exception("运行出错: %s", e)
            # 出错时也保存检查点
            self.ckpt_mgr.save(self.state)
            return 1


# ============================================================
# 命令行入口
# ============================================================

def get_checkpoint_dir_from_env():
    """
    从环境变量获取检查点目录
    """
    chkpnt_dir = os.environ.get("LSB_CHKPNT_DIR")
    if chkpnt_dir:
        return chkpnt_dir

    # 兼容旧版变量
    chkpnt_dir = os.environ.get("LSB_CHECKPOINT_DIR")
    return chkpnt_dir


def main():
    parser = argparse.ArgumentParser(
        description="应用级检查点演示程序",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 首次运行，信号模式
  %(prog.py --steps 200 --interval 0.5 /path/to/checkpoint_dir

  # 从检查点恢复
  %(prog)s -r /path/to/checkpoint_dir

  # 文件触发模式
  %(prog)s --mode file /path/to/checkpoint_dir

环境变量:
  LSB_CHKPNT_DIR   LSF 检查点目录（自动检测）
  LSB_JOBID         LSF 作业 ID
        """,
    )

    parser.add_argument(
        "-r", "--restart",
        action="store_true",
        help="从检查点恢复运行（LSF erestart 调用时使用）",
    )
    parser.add_argument(
        "--steps",
        type=int,
        default=600,
        help="总计算步数 (默认: 600)",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="每步之间的间隔秒数 (默认: 1.0)",
    )
    parser.add_argument(
        "--mode",
        choices=["signal", "file"],
        default="signal",
        help="检查点触发模式: signal(信号) 或 file(触发文件) (默认: signal)",
    )
    parser.add_argument(
        "checkpoint_dir",
        nargs="?",
        default=None,
        help="检查点目录（默认从 LSB_CHKPNT_DIR 环境变量获取）",
    )

    args = parser.parse_args()

    # 确定检查点目录
    checkpoint_dir = args.checkpoint_dir or get_checkpoint_dir_from_env()
    if not checkpoint_dir:
        # 使用当前目录下的默认路径
        checkpoint_dir = os.path.join(os.getcwd(), "checkpoint_data")
        logger.warning("未指定检查点目录，使用默认目录: %s", checkpoint_dir)

    # 创建应用实例
    app = CheckpointDemoApp(
        checkpoint_dir=checkpoint_dir,
        mode=args.mode,
        total_steps=args.steps,
        interval=args.interval,
    )

    # 如果是重启模式，尝试从检查点恢复
    if args.restart:
        if not app.restore_from_checkpoint():
            logger.error("未找到检查点文件，无法恢复")
            sys.exit(1)

    # 运行
    exit_code = app.run()
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
