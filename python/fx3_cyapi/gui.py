"""PySide6 monitor: all USB requests and payload ownership stay in C++."""
import sys
import numpy as np
from PySide6.QtCore import QThread, QTimer, Qt
from PySide6.QtGui import QColor, QPainter, QPolygonF
from PySide6.QtCore import QPointF
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QFormLayout, QHBoxLayout, QLabel, QMainWindow,
    QMessageBox, QPushButton, QSpinBox, QVBoxLayout, QWidget,
)
from . import AcquisitionConfig, AcquisitionEngine


class Command(QThread):
    """Lifecycle operations may wait for the driver; keep them off the GUI thread."""
    def __init__(self, action, parent):
        super().__init__(parent)
        self.action = action
        self.error = None

    def run(self):
        try:
            self.action()
        except Exception as exc:
            self.error = str(exc)


class Preview(QWidget):
    def __init__(self):
        super().__init__()
        self.values = np.empty(0, dtype=np.uint8)
        self.setMinimumHeight(200)

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#142333"))
        painter.setPen(QColor("#69d7c6"))
        if len(self.values) > 1:
            w, h = self.width() - 20, self.height() - 20
            points = QPolygonF([
                QPointF(10 + i * w / (len(self.values) - 1), 10 + (255 - int(v)) * h / 255)
                for i, v in enumerate(self.values)
            ])
            painter.drawPolyline(points)


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("FX3 acquisition monitor")
        self.resize(900, 600)
        self.engine = AcquisitionEngine()
        self.job = None
        self.closing = False
        self.close_ready = False
        self.simulated = QCheckBox("模拟采集（无需 USB 设备）")
        self.simulated.setChecked(True)
        self.index = self.spin(0, 255, 0)
        self.endpoint = self.spin(0x81, 0x8f, 0x81)
        self.endpoint.setDisplayIntegerBase(16)
        self.endpoint.setPrefix("0x")
        self.block_kib = self.spin(1, 16384, 1024)
        self.depth = self.spin(1, 64, 8)
        self.capacity = self.spin(1, 1024, 16)
        self.timeout = self.spin(1, 60000, 1000)
        self.start_button = QPushButton("开始")
        self.stop_button = QPushButton("停止 / 关闭")
        self.stop_button.setEnabled(False)
        self.start_button.clicked.connect(self.start)
        self.stop_button.clicked.connect(lambda: self.dispatch(self.engine.close))
        self.status = QLabel("就绪；预览显示原始 uint8 字节，不代表 ADC 电压或采样时间。")
        self.status.setWordWrap(True)
        self.preview = Preview()
        form = QFormLayout()
        form.addRow(self.simulated)
        for title, widget in [("设备索引", self.index), ("Bulk IN 端点", self.endpoint),
                              ("每块 KiB", self.block_kib), ("在途请求数", self.depth),
                              ("完成队列容量", self.capacity), ("请求超时 ms", self.timeout)]:
            form.addRow(title, widget)
        buttons = QHBoxLayout()
        buttons.addWidget(self.start_button)
        buttons.addWidget(self.stop_button)
        layout = QVBoxLayout()
        layout.addLayout(form)
        layout.addLayout(buttons)
        layout.addWidget(self.status)
        layout.addWidget(self.preview)
        root = QWidget()
        root.setLayout(layout)
        self.setCentralWidget(root)
        self.controls = [self.simulated, self.index, self.endpoint, self.block_kib,
                         self.depth, self.capacity, self.timeout]
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(50)

    @staticmethod
    def spin(low, high, value):
        result = QSpinBox()
        result.setRange(low, high)
        result.setValue(value)
        return result

    def dispatch(self, action):
        if self.job is not None:
            return
        self.start_button.setEnabled(False)
        self.stop_button.setEnabled(False)
        for widget in self.controls:
            widget.setEnabled(False)
        self.job = Command(action, self)
        self.job.finished.connect(self.command_finished)
        self.job.start()

    def command_finished(self):
        job, self.job = self.job, None
        error = job.error
        job.deleteLater()
        if self.closing:
            if self.engine.stats().opened:
                self.dispatch(self.engine.close)
            else:
                self.close_ready = True
                self.close()
            return
        if error:
            self.status.setText(error)
            QMessageBox.warning(self, "采集错误", error)
        self.update_controls()

    def update_controls(self):
        opened = self.engine.stats().opened
        self.start_button.setEnabled(not opened)
        self.stop_button.setEnabled(opened)
        for widget in self.controls:
            widget.setEnabled(not opened)

    def start(self):
        config = AcquisitionConfig()
        config.block_size = self.block_kib.value() * 1024
        config.queue_depth = self.depth.value()
        config.queue_capacity = self.capacity.value()
        config.timeout_ms = self.timeout.value()
        index, endpoint, simulated = self.index.value(), self.endpoint.value(), self.simulated.isChecked()

        def action():
            try:
                self.engine.open(index=index, endpoint=endpoint, simulated=simulated)
                self.engine.configure(config)
                self.engine.start()
            except Exception:
                self.engine.close()
                raise
        self.dispatch(action)

    def refresh(self):
        if self.job is not None or self.closing:
            return
        try:
            # At most 32 whole blocks per tick; never dispatch individual USB packets.
            # Keep only a small copied display preview, releasing pooled native views.
            for _ in range(32):
                block = self.engine.read_block(timeout_ms=0)
                if block is None:
                    break
                view = np.frombuffer(block, dtype=np.uint8)
                stride = max(1, (len(view) + 1023) // 1024)
                self.preview.values = view[::stride].copy()
                del view, block
            stats = self.engine.stats()
            rate = stats.received_bytes / max(stats.elapsed_seconds, 1e-9) / (1024 * 1024)
            state = "采集中" if stats.running else "已停止"
            self.status.setText(
                f"{state} | {rate:.1f} MiB/s | 收到 {stats.received_blocks} 块 | "
                f"排队 {stats.queued_blocks} | 主机丢弃 {stats.dropped_blocks} 块 | "
                f"池 {stats.pool_bytes / (1024*1024):.1f} MiB | 错误 {stats.errors}"
                + (f" | {stats.last_error}" if stats.last_error else "")
            )
            self.preview.update()
        except Exception as exc:
            self.status.setText(str(exc))
            self.dispatch(self.engine.close)

    def closeEvent(self, event):
        if self.close_ready:
            event.accept()
            return
        event.ignore()
        self.closing = True
        self.timer.stop()
        if self.job is None:
            self.dispatch(self.engine.close)


def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
